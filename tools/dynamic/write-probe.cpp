// Stage 6 B-side: measure write acknowledgement and visibility.
//
// The engine tests prove the protocol; this probe measures what the protocol
// costs. Three numbers matter and they are not the same:
//
// * acknowledgement latency: how long Insert takes to return a receipt,
// * visibility: whether the very next Get can read the acknowledged record,
// * recovery time: how long reopening an index with a WAL history takes.
//
// Output is a single JSON object on stdout so a driver script can turn several
// runs into evidence without parsing prose.

#include "agent_io/engine.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace agent_io;

struct Options {
    std::filesystem::path index;
    std::uint32_t dimension = 16;
    std::uint32_t writes = 100;
    std::uint32_t queries = 10;
    std::uint32_t k = 10;
    std::uint32_t seed = 7;
    // Measure recovery only. A second process opening the same history is the
    // only honest way to time replay: an in-process reopen shares page cache.
    bool reopen_only = false;
};

std::uint32_t NextRandom(std::uint64_t& state) {
    state += 0x9e3779b97f4a7c15ull;
    std::uint64_t value = state;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    value ^= value >> 31;
    return static_cast<std::uint32_t>(value >> 32);
}

double NextUnit(std::uint64_t& state) {
    return static_cast<double>(NextRandom(state)) / 4294967296.0;
}

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Percentiles {
    std::uint64_t p50 = 0;
    std::uint64_t p95 = 0;
    std::uint64_t p99 = 0;
    std::uint64_t maximum = 0;
    std::uint64_t total = 0;
};

Percentiles Summarize(std::vector<std::uint64_t> samples) {
    Percentiles result;
    if (samples.empty()) return result;
    std::sort(samples.begin(), samples.end());
    result.maximum = samples.back();
    const auto pick = [&samples](double fraction) {
        const auto raw = static_cast<std::size_t>(fraction * static_cast<double>(samples.size()));
        const auto index = (std::min)(raw, samples.size() - 1);
        return samples[index];
    };
    result.p50 = pick(0.50);
    result.p95 = pick(0.95);
    result.p99 = pick(0.99);
    std::uint64_t total = 0;
    for (const auto sample : samples) total += sample;
    result.total = total;
    return result;
}

EngineOptions DefaultOptions() {
    EngineOptions options;
    options.memory.total_bytes = 256u << 20;
    options.memory.cache_bytes = 16u << 20;
    options.memory.delta_bytes = 64u << 20;
    options.memory.metadata_bytes = 16u << 20;
    options.memory.workspace_bytes = 128u << 20;
    options.memory.reserve_bytes = 32u << 20;
    options.block_bytes = 4096;
    options.max_queries = 4;
    options.max_inflight_reads = 64;
    options.direct_reads = true;
    return options;
}

bool ParseOptions(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string flag = argv[index];
        const auto next = [&argc, &argv, &index]() -> const char* {
            return index + 1 < argc ? argv[++index] : nullptr;
        };
        if (flag == "--index") {
            const auto* value = next();
            if (!value) return false;
            options.index = value;
        } else if (flag == "--dimension") {
            const auto* value = next();
            if (!value) return false;
            options.dimension = static_cast<std::uint32_t>(std::atoi(value));
        } else if (flag == "--writes") {
            const auto* value = next();
            if (!value) return false;
            options.writes = static_cast<std::uint32_t>(std::atoi(value));
        } else if (flag == "--queries") {
            const auto* value = next();
            if (!value) return false;
            options.queries = static_cast<std::uint32_t>(std::atoi(value));
        } else if (flag == "--k") {
            const auto* value = next();
            if (!value) return false;
            options.k = static_cast<std::uint32_t>(std::atoi(value));
        } else if (flag == "--seed") {
            const auto* value = next();
            if (!value) return false;
            options.seed = static_cast<std::uint32_t>(std::atoi(value));
        } else if (flag == "--reopen-only") {
            options.reopen_only = true;
        } else {
            return false;
        }
    }
    if (options.index.empty() || options.dimension == 0) return false;
    return options.reopen_only || options.writes > 0;
}

void PrintPercentiles(const char* name, const Percentiles& values, bool trailing_comma) {
    std::printf("  \"%s\": {\"p50\": %llu, \"p95\": %llu, \"p99\": %llu, "
                "\"max\": %llu, \"total\": %llu}%s\n",
                name,
                static_cast<unsigned long long>(values.p50),
                static_cast<unsigned long long>(values.p95),
                static_cast<unsigned long long>(values.p99),
                static_cast<unsigned long long>(values.maximum),
                static_cast<unsigned long long>(values.total),
                trailing_comma ? "," : "");
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        std::fprintf(stderr,
                     "usage: write_probe --index DIR [--dimension N] [--writes N] "
                     "[--queries N] [--k N] [--seed N]\n");
        return 2;
    }

    const auto open_started = NowNs();
    auto opened = Engine::Open(options.index, DefaultOptions());
    const auto open_ns = NowNs() - open_started;
    if (std::holds_alternative<Error>(opened)) {
        std::fprintf(stderr, "open failed: %s\n", std::get<Error>(opened).message.c_str());
        return 1;
    }
    auto engine = std::get<std::unique_ptr<Engine>>(std::move(opened));
    const auto initial = engine->Stats();
    if (std::holds_alternative<Error>(initial)) {
        std::fprintf(stderr, "stats failed: %s\n", std::get<Error>(initial).message.c_str());
        return 1;
    }
    const Sequence recovered_sequence = std::get<EngineStats>(initial).published_sequence;
    const std::uint64_t recovered_live = std::get<EngineStats>(initial).live_vectors;

    if (options.reopen_only) {
        const auto closed = engine->Close();
        std::printf("{\n");
        std::printf("  \"schema_version\": 1,\n");
        std::printf("  \"probe\": \"stage6_reopen\",\n");
        std::printf("  \"index\": \"%s\",\n", options.index.generic_string().c_str());
        std::printf("  \"open_ns\": %llu,\n", static_cast<unsigned long long>(open_ns));
        std::printf("  \"recovered_sequence\": %llu,\n",
                    static_cast<unsigned long long>(recovered_sequence));
        std::printf("  \"recovered_live\": %llu,\n",
                    static_cast<unsigned long long>(recovered_live));
        std::printf("  \"closed\": %s\n",
                    std::holds_alternative<Success>(closed) ? "true" : "false");
        std::printf("}\n");
        return std::holds_alternative<Success>(closed) ? 0 : 1;
    }

    std::uint64_t state = options.seed;
    std::vector<std::uint64_t> write_latency;
    std::vector<std::uint64_t> visibility_latency;
    std::vector<std::uint64_t> query_latency;
    write_latency.reserve(options.writes);
    visibility_latency.reserve(options.writes);
    query_latency.reserve(options.queries);

    std::uint32_t visible = 0;
    std::uint32_t confirmed = 0;
    std::uint32_t mismatched = 0;
    Sequence first_sequence = 0;
    Sequence last_sequence = 0;
    const VectorId base = 7000000;

    for (std::uint32_t index = 0; index < options.writes; ++index) {
        std::vector<float> values(options.dimension);
        for (auto& value : values) value = static_cast<float>(NextUnit(state) * 2.0 - 1.0);
        const VectorId id = base + index;
        const auto started = NowNs();
        const auto receipt = engine->Insert(id, values, WriteOptions{});
        write_latency.push_back(NowNs() - started);
        if (std::holds_alternative<Error>(receipt)) {
            std::fprintf(stderr, "insert failed at %u: %s\n", index,
                         std::get<Error>(receipt).message.c_str());
            return 1;
        }
        const auto sequence = std::get<WriteReceipt>(receipt).sequence;
        if (index == 0) first_sequence = sequence;
        last_sequence = sequence;
        ++confirmed;

        // The receipt is a promise. Reading it back immediately is the only
        // check that the promise was kept rather than merely returned.
        const auto read_started = NowNs();
        const auto stored = engine->Get(id);
        visibility_latency.push_back(NowNs() - read_started);
        if (std::holds_alternative<StoredVector>(stored)) {
            const auto& vector = std::get<StoredVector>(stored);
            if (vector.version == sequence &&
                vector.values.size() == values.size() &&
                std::equal(vector.values.begin(), vector.values.end(), values.begin()))
                ++visible;
            else
                ++mismatched;
        }
    }

    for (std::uint32_t index = 0; index < options.queries; ++index) {
        std::vector<float> query(options.dimension);
        for (auto& value : query) value = static_cast<float>(NextUnit(state) * 2.0 - 1.0);
        SearchOptions search;
        search.k = options.k;
        search.candidate_limit = (std::max)(options.k * 4, 64u);
        const auto started = NowNs();
        const auto result = engine->Search(query, search);
        query_latency.push_back(NowNs() - started);
        if (std::holds_alternative<Error>(result)) {
            std::fprintf(stderr, "search failed: %s\n",
                         std::get<Error>(result).message.c_str());
            return 1;
        }
    }

    const auto final_stats = engine->Stats();
    if (std::holds_alternative<Error>(final_stats)) return 1;
    const auto& stats = std::get<EngineStats>(final_stats);
    const auto closed = engine->Close();
    if (std::holds_alternative<Error>(closed)) {
        std::fprintf(stderr, "close failed: %s\n", std::get<Error>(closed).message.c_str());
        return 1;
    }

    // Reopening is the expensive part of recovery: it replays every record above
    // the checkpoint sequence. Measuring it here keeps the claim honest instead
    // of inferring recovery cost from a warm in-process read.
    const auto reopen_started = NowNs();
    auto reopened = Engine::Open(options.index, DefaultOptions());
    const auto reopen_ns = NowNs() - reopen_started;
    if (std::holds_alternative<Error>(reopened)) {
        std::fprintf(stderr, "reopen failed: %s\n", std::get<Error>(reopened).message.c_str());
        return 1;
    }
    auto second = std::get<std::unique_ptr<Engine>>(std::move(reopened));
    const auto second_stats = second->Stats();
    if (std::holds_alternative<Error>(second_stats)) return 1;
    const auto& after = std::get<EngineStats>(second_stats);
    std::uint32_t survived = 0;
    for (std::uint32_t index = 0; index < options.writes; ++index)
        if (std::holds_alternative<StoredVector>(second->Get(base + index))) ++survived;
    static_cast<void>(second->Close());

    const auto write_summary = Summarize(write_latency);
    const auto visibility_summary = Summarize(visibility_latency);
    const auto query_summary = Summarize(query_latency);

    std::printf("{\n");
    std::printf("  \"schema_version\": 1,\n");
    std::printf("  \"probe\": \"stage6_write_probe\",\n");
    std::printf("  \"index\": \"%s\",\n", options.index.generic_string().c_str());
    std::printf("  \"dimension\": %u,\n", options.dimension);
    std::printf("  \"k\": %u,\n", options.k);
    PrintPercentiles("write_acknowledge_ns", write_summary, true);
    PrintPercentiles("visible_read_ns", visibility_summary, true);
    PrintPercentiles("query_ns", query_summary, true);
    std::printf("  \"open_ns\": %llu,\n", static_cast<unsigned long long>(open_ns));
    std::printf("  \"reopen_ns\": %llu,\n", static_cast<unsigned long long>(reopen_ns));
    std::printf("  \"recovered_sequence_at_open\": %llu,\n",
                static_cast<unsigned long long>(recovered_sequence));
    std::printf("  \"recovered_live_at_open\": %llu,\n",
                static_cast<unsigned long long>(recovered_live));
    std::printf("  \"confirmed_writes\": %u,\n", confirmed);
    std::printf("  \"first_sequence\": %llu,\n", static_cast<unsigned long long>(first_sequence));
    std::printf("  \"last_sequence\": %llu,\n", static_cast<unsigned long long>(last_sequence));
    std::printf("  \"immediately_visible\": %u,\n", visible);
    std::printf("  \"visibility_mismatches\": %u,\n", mismatched);
    std::printf("  \"published_sequence\": %llu,\n",
                static_cast<unsigned long long>(stats.published_sequence));
    std::printf("  \"live_vectors\": %llu,\n", static_cast<unsigned long long>(stats.live_vectors));
    std::printf("  \"wal_bytes_written\": %llu,\n",
                static_cast<unsigned long long>(stats.wal_bytes_written));
    std::printf("  \"logical_update_bytes\": %llu,\n",
                static_cast<unsigned long long>(stats.logical_update_bytes));
    std::printf("  \"delta_resident_bytes\": %llu,\n",
                static_cast<unsigned long long>(stats.delta_resident_bytes));
    std::printf("  \"sequence_after_reopen\": %llu,\n",
                static_cast<unsigned long long>(after.published_sequence));
    std::printf("  \"survived_reopen\": %u\n", survived);
    std::printf("}\n");
    return 0;
}
