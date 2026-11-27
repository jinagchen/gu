// Stage 7 B checks: checkpoint, generation switch, compaction and reclamation.
//
// The acceptance condition is narrow: what a query answered before a checkpoint
// it must still answer after one, the files a merge supersedes must stop
// existing, and the WAL must not keep records that are already in a segment.

#include "agent_io/engine.hpp"
#include "agent_io/index.hpp"
#include "agent_io/storage.hpp"

#include "../../src/index/builder.hpp"
#include "../../src/index/delta.hpp"
#include "../../src/index/reader.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace agent_io;

int g_checks = 0;
int g_failures = 0;

void Require(bool condition, const char* message) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

const Error* AsError(const Result<Success>& result) {
    return std::holds_alternative<Error>(result) ? &std::get<Error>(result) : nullptr;
}

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

double SquaredL2(const float* left, const float* right, std::uint32_t dimension) {
    double total = 0.0;
    for (std::uint32_t index = 0; index < dimension; ++index) {
        const double delta = static_cast<double>(left[index]) - static_cast<double>(right[index]);
        total += delta * delta;
    }
    return total;
}

struct Fixture {
    std::filesystem::path root;
    std::filesystem::path index;
    std::filesystem::path staging;
    std::vector<float> values;
    std::vector<VectorId> identifiers;
    std::uint32_t dimension = 8;
    std::uint32_t count = 384;
};

std::uint64_t g_serial = 0;

Fixture Prepare(const std::string& name) {
    Fixture fixture;
    fixture.root = std::filesystem::temp_directory_path() /
        ("agent_io_maintenance_" + name + "_" + std::to_string(GetCurrentProcessId()) + "_" +
         std::to_string(++g_serial));
    std::error_code error;
    std::filesystem::remove_all(fixture.root, error);
    std::filesystem::create_directories(fixture.root, error);
    fixture.index = fixture.root / "index";
    fixture.staging = fixture.root / "index.staging";

    const std::uint32_t clusters = 12;
    std::uint64_t seed = 11;
    std::vector<float> centers(static_cast<std::size_t>(clusters) * fixture.dimension);
    for (auto& value : centers) value = static_cast<float>(NextUnit(seed) * 4.0 - 2.0);
    fixture.values.assign(static_cast<std::size_t>(fixture.count) * fixture.dimension, 0.0f);
    fixture.identifiers.resize(fixture.count);
    for (std::uint32_t index = 0; index < fixture.count; ++index) {
        const std::uint32_t cluster = index % clusters;
        for (std::uint32_t component = 0; component < fixture.dimension; ++component) {
            const double noise = (NextUnit(seed) - 0.5) * 0.05;
            fixture.values[static_cast<std::size_t>(index) * fixture.dimension + component] =
                centers[static_cast<std::size_t>(cluster) * fixture.dimension + component] +
                static_cast<float>(noise);
        }
        fixture.identifiers[index] = 500u + index * 3u;
    }

    const auto vectors_path = fixture.root / "vectors.f32";
    const auto ids_path = fixture.root / "ids.u64";
    {
        std::ofstream vectors(vectors_path, std::ios::binary | std::ios::trunc);
        vectors.write(reinterpret_cast<const char*>(fixture.values.data()),
                      static_cast<std::streamsize>(fixture.values.size() * sizeof(float)));
    }
    {
        std::ofstream ids(ids_path, std::ios::binary | std::ios::trunc);
        ids.write(reinterpret_cast<const char*>(fixture.identifiers.data()),
                  static_cast<std::streamsize>(fixture.identifiers.size() * sizeof(VectorId)));
    }

    FirstBuildRequest request;
    request.vectors_path = vectors_path;
    request.ids_path = ids_path;
    request.dimension = fixture.dimension;
    request.max_degree = 16;
    request.build_beam = 4;
    request.vector_count = fixture.count;
    request.staging_directory = fixture.staging;
    request.output_directory = fixture.index;
    request.build_id = "maintenance-test";
    request.native_parameters = "R16 exact-knn baseline";
    request.native_version = "test-native-1.0";
    request.min_free_disk_bytes = 0;
    const auto built = RunFirstBuild(request);
    if (std::holds_alternative<Error>(built))
        std::fprintf(stderr, "  fixture build error: %s\n",
                     std::get<Error>(built).message.c_str());
    Require(std::holds_alternative<BuildOutcome>(built), "fixture index builds");
    return fixture;
}

EngineOptions Options(std::uint64_t delta_bytes = 8u << 20, bool maintenance = false) {
    EngineOptions options;
    const std::uint64_t total = 96u << 20;
    const std::uint64_t cache = 8u << 20;
    const std::uint64_t metadata = 8u << 20;
    const std::uint64_t reserve = 8u << 20;
    options.memory.total_bytes = total;
    options.memory.cache_bytes = cache;
    options.memory.delta_bytes = delta_bytes;
    options.memory.metadata_bytes = metadata;
    options.memory.workspace_bytes = total - cache - delta_bytes - metadata - reserve;
    options.memory.reserve_bytes = reserve;
    options.block_bytes = 4096;
    options.max_queries = 4;
    options.max_inflight_reads = 64;
    options.direct_reads = true;
    if (maintenance) {
        // A rebuild budget has to name the same total as the query budget; the
        // governor refuses anything else rather than shifting memory silently.
        MemoryBudget rebuild;
        rebuild.total_bytes = total;
        rebuild.cache_bytes = 0;
        rebuild.delta_bytes = 0;
        rebuild.metadata_bytes = metadata;
        rebuild.reserve_bytes = reserve;
        rebuild.workspace_bytes = total - metadata - reserve;
        options.rebuild_memory = rebuild;
        options.max_delta_segments = 2;
        options.checkpoint_trigger_vectors = 8;
        options.maintenance_interval = std::chrono::milliseconds(10);
    }
    return options;
}

// One maintenance policy, expressed only through EngineOptions so an ablation
// arm differs from the shipped default by nothing but these numbers.
EngineOptions PolicyOptions(std::uint32_t trigger, std::uint32_t max_segments, bool settle,
                            std::uint64_t merge_min_bytes) {
    EngineOptions options = Options(8u << 20, true);
    options.checkpoint_trigger_vectors = trigger;
    options.max_delta_segments = max_segments;
    options.maintenance_settle_flush = settle;
    options.merge_min_bytes = merge_min_bytes;
    return options;
}

std::unique_ptr<Engine> OpenFixture(const Fixture& fixture, const EngineOptions& options) {
    auto opened = Engine::Open(fixture.index, options);
    if (std::holds_alternative<Error>(opened))
        std::fprintf(stderr, "  open error: %s\n",
                     std::get<Error>(opened).message.c_str());
    Require(std::holds_alternative<std::unique_ptr<Engine>>(opened), "engine opens");
    if (!std::holds_alternative<std::unique_ptr<Engine>>(opened)) return nullptr;
    return std::get<std::unique_ptr<Engine>>(std::move(opened));
}

std::vector<float> VectorFor(const Fixture& fixture, std::uint32_t index) {
    std::vector<float> values(fixture.dimension);
    for (std::uint32_t component = 0; component < fixture.dimension; ++component)
        values[component] =
            fixture.values[static_cast<std::size_t>(index) * fixture.dimension + component];
    return values;
}

// A vector that is deliberately far from every fixture cluster, so a merged
// segment cannot pass by accident: only the write itself can produce it.
std::vector<float> FarVector(const Fixture& fixture, std::uint32_t index) {
    std::vector<float> values(fixture.dimension);
    for (std::uint32_t component = 0; component < fixture.dimension; ++component) {
        const double base = static_cast<double>(component + 1) * 1.5;
        const double offset = static_cast<double>(index) * 0.25;
        values[component] = static_cast<float>(base + offset);
    }
    return values;
}

std::vector<std::filesystem::path> WalFiles(const Fixture& fixture) {
    std::vector<std::filesystem::path> paths;
    const auto directory = fixture.index / "wal";
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return paths;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) break;
        if (!entry.is_regular_file(error) || error) continue;
        if (entry.path().extension() == ".log") paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

std::uint32_t SegmentDirectories(const Fixture& fixture) {
    std::uint32_t count = 0;
    const auto directory = fixture.index / "segments";
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) break;
        if (entry.is_directory(error) && !error) ++count;
    }
    return count;
}

std::vector<VectorId> QueryIds(Engine& engine, const std::vector<float>& query,
                              std::uint32_t k) {
    SearchOptions options;
    options.k = k;
    options.candidate_limit = 64;
    auto found = engine.Search(query, options);
    std::vector<VectorId> ids;
    if (const auto* error = std::get_if<Error>(&found)) {
        std::fprintf(stderr, "  search error: %s\n", error->message.c_str());
        return ids;
    }
    for (const auto& neighbour : std::get<SearchResult>(found).neighbors)
        ids.push_back(neighbour.id);
    return ids;
}

bool WaitFor(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

// --- cases ---------------------------------------------------------------

void TestCheckpointPublishesWithoutChangingAnswers() {
    auto fixture = Prepare("publish");
    auto engine = OpenFixture(fixture, Options());
    if (!engine) return;

    const std::vector<float> query = VectorFor(fixture, 7);
    const auto before = QueryIds(*engine, query, 5);

    const auto distant = FarVector(fixture, 0);
    const auto write = engine->Insert(910001, distant, WriteOptions{});
    Require(std::holds_alternative<WriteReceipt>(write), "checkpoint fixture write");
    const auto published = std::get<WriteReceipt>(write).sequence;

    const auto receipt = engine->Checkpoint(std::chrono::milliseconds(5000));
    Require(std::holds_alternative<CheckpointReceipt>(receipt), "checkpoint succeeds");
    if (std::holds_alternative<CheckpointReceipt>(receipt)) {
        const auto& value = std::get<CheckpointReceipt>(receipt);
        Require(value.target_sequence == published, "checkpoint targets the published sequence");
        Require(value.checkpoint_sequence == published,
                "checkpoint publishes the sequence it flushed");
        Require(value.manifest_generation >= 2, "checkpoint advanced the manifest generation");
    }

    const auto stats = engine->Stats();
    if (std::holds_alternative<EngineStats>(stats)) {
        const auto& value = std::get<EngineStats>(stats);
        Require(value.merge_backlog_bytes == 0, "the delta has no unflushed backlog afterwards");
        Require(value.checkpoint_bytes_written > 0, "checkpoint bytes are disclosed");
    }

    const auto after = QueryIds(*engine, query, 5);
    Require(before == after, "a checkpoint does not change the answer to the same query");

    const auto search = engine->Search(query, SearchOptions{});
    if (std::holds_alternative<SearchResult>(search))
        Require(std::get<SearchResult>(search).metrics.delta_records_scanned == 0,
                "checkpointed records are read from the segment, not rescanned");

    const auto stored = engine->Get(910001);
    Require(std::holds_alternative<StoredVector>(stored),
            "a checkpointed write stays readable");
    if (std::holds_alternative<StoredVector>(stored))
        Require(std::get<StoredVector>(stored).version == published,
                "the checkpointed version is the one that was written");
    static_cast<void>(engine->Close());
}

void TestRestartAfterCheckpointDoesNotReplay() {
    auto fixture = Prepare("restart");
    EngineStats before{};
    {
        auto engine = OpenFixture(fixture, Options());
        if (!engine) return;
        for (std::uint32_t index = 0; index < 6; ++index) {
            const auto write = engine->Insert(920000 + index, FarVector(fixture, index),
                                              WriteOptions{});
            Require(std::holds_alternative<WriteReceipt>(write), "restart fixture write");
        }
        const auto receipt = engine->Checkpoint(std::chrono::milliseconds(5000));
        Require(std::holds_alternative<CheckpointReceipt>(receipt), "restart fixture checkpoint");
        const auto stats = engine->Stats();
        if (std::holds_alternative<EngineStats>(stats)) before = std::get<EngineStats>(stats);
        static_cast<void>(engine->Close());
    }

    // The records are in a segment now, so the WAL has nothing left to replay.
    // The writer keeps one empty file open at the new epoch; that is expected.
    Require(WalFiles(fixture).size() <= 1, "checkpoint reclaims the WAL epoch it closed");

    {
        auto engine = OpenFixture(fixture, Options());
        if (!engine) return;
        const auto stats = engine->Stats();
        if (std::holds_alternative<EngineStats>(stats)) {
            const auto& value = std::get<EngineStats>(stats);
            Require(value.published_sequence == before.published_sequence,
                    "restart keeps the published sequence");
            Require(value.checkpoint_sequence == before.published_sequence,
                    "restart sees the checkpoint the segment represents");
            Require(value.live_vectors == before.live_vectors, "restart keeps the live count");
        }
        for (std::uint32_t index = 0; index < 6; ++index) {
            const auto stored = engine->Get(920000 + index);
            Require(std::holds_alternative<StoredVector>(stored),
                    "checkpointed writes survive a restart without replay");
        }
        const auto next = engine->Insert(920100, FarVector(fixture, 9), WriteOptions{});
        Require(std::holds_alternative<WriteReceipt>(next), "writes continue after a checkpoint");
        if (std::holds_alternative<WriteReceipt>(next))
            Require(std::get<WriteReceipt>(next).sequence == before.published_sequence + 1,
                    "the sequence continues from the checkpoint instead of restarting");
        static_cast<void>(engine->Close());
    }
}

void TestMergesKeepSegmentsBounded() {
    auto fixture = Prepare("merge");
    const EngineOptions options = Options(8u << 20, true);
    auto engine = OpenFixture(fixture, options);
    if (!engine) return;

    // Each round waits for the background flush to cover it, so the segment
    // budget is actually exceeded instead of one late flush swallowing the lot.
    for (std::uint32_t round = 0; round < 8; ++round) {
        for (std::uint32_t index = 0; index < 10; ++index) {
            const auto write = engine->Insert(930000 + round * 10 + index,
                                              FarVector(fixture, round * 10 + index),
                                              WriteOptions{});
            Require(std::holds_alternative<WriteReceipt>(write), "merge fixture write");
        }
        const Sequence expected = (round + 1) * 10;
        const bool flushed = WaitFor(
            [&] {
                const auto stats = engine->Stats();
                return std::holds_alternative<EngineStats>(stats) &&
                       std::get<EngineStats>(stats).checkpoint_sequence >= expected;
            },
            std::chrono::seconds(20));
        Require(flushed, "the background flush keeps up with the write rounds");
    }

    const bool merged = WaitFor(
        [&] {
            const auto stats = engine->Stats();
            return std::holds_alternative<EngineStats>(stats) &&
                   std::get<EngineStats>(stats).merge_bytes_written > 0;
        },
        std::chrono::seconds(30));
    Require(merged, "a merge actually wrote a segment");

    const bool bounded = WaitFor(
        [&] { return SegmentDirectories(fixture) <= 3; }, std::chrono::seconds(30));
    Require(bounded, "background merges keep the segment count within the budget");

    const auto stats = engine->Stats();
    if (std::holds_alternative<EngineStats>(stats)) {
        const auto& value = std::get<EngineStats>(stats);
        Require(value.merge_backlog_bytes == 0, "the backlog is drained once merging keeps up");
        Require(value.published_sequence == 80, "every acknowledged write kept its sequence");
        Require(value.live_vectors == fixture.count + 80, "the live count survives the merges");
    }

    // Nothing may be lost to a merge: every ID is still readable.
    std::uint32_t readable = 0;
    for (std::uint32_t index = 0; index < 80; ++index) {
        if (std::holds_alternative<StoredVector>(engine->Get(930000 + index))) ++readable;
    }
    Require(readable == 80, "every write survives repeated merges");
    static_cast<void>(engine->Close());
}

void TestQueryResultsSurviveMerge() {
    auto fixture = Prepare("queries");
    const EngineOptions options = Options(8u << 20, false);
    auto engine = OpenFixture(fixture, options);
    if (!engine) return;

    std::vector<std::vector<float>> queries;
    for (std::uint32_t index = 0; index < 6; ++index)
        queries.push_back(VectorFor(fixture, index * 37 + 5));

    std::vector<std::vector<VectorId>> before;
    for (const auto& query : queries) before.push_back(QueryIds(*engine, query, 10));
    Require(!before.empty() && !before.front().empty(), "the fixture answers before a merge");

    for (std::uint32_t round = 0; round < 4; ++round) {
        for (std::uint32_t index = 0; index < 8; ++index) {
            static_cast<void>(engine->Insert(940000 + round * 8 + index,
                                             FarVector(fixture, round * 8 + index),
                                             WriteOptions{}));
        }
        const auto receipt = engine->Checkpoint(std::chrono::milliseconds(5000));
        Require(std::holds_alternative<CheckpointReceipt>(receipt), "query fixture checkpoint");
    }

    // The queries sit in the base clusters, so their answers must not move just
    // because the writes were merged into new segments.
    for (std::size_t index = 0; index < queries.size(); ++index) {
        const auto after = QueryIds(*engine, queries[index], 10);
        Require(after == before[index],
                "a merge does not change the answer to the same query");
    }
    static_cast<void>(engine->Close());
}

void TestBackgroundMaintenanceFlushesWithoutACall() {
    auto fixture = Prepare("background");
    const EngineOptions options = Options(8u << 20, true);
    auto engine = OpenFixture(fixture, options);
    if (!engine) return;

    for (std::uint32_t index = 0; index < 24; ++index) {
        const auto write = engine->Insert(950000 + index, FarVector(fixture, index),
                                          WriteOptions{});
        Require(std::holds_alternative<WriteReceipt>(write), "background fixture write");
    }

    const bool flushed = WaitFor(
        [&] {
            const auto stats = engine->Stats();
            return std::holds_alternative<EngineStats>(stats) &&
                   std::get<EngineStats>(stats).checkpoint_sequence >= 24;
        },
        std::chrono::seconds(30));
    Require(flushed, "background maintenance flushes the delta without an explicit checkpoint");

    const auto stats = engine->Stats();
    if (std::holds_alternative<EngineStats>(stats)) {
        const auto& value = std::get<EngineStats>(stats);
        Require(value.merge_backlog_bytes == 0, "the background flush drains the backlog");
        Require(value.live_vectors == fixture.count + 24, "the live count survives the flush");
    }
    static_cast<void>(engine->Close());
}

// A merge rewrites the whole index, so the one thing it must never do is lose a
// row. Restarting after a merge exercises the published manifest as the only
// record of what the merged segment contains.
void TestMergeSurvivesRestart() {
    auto fixture = Prepare("merge-restart");
    EngineStats closed_stats{};
    std::uint32_t closed_directories = 0;
    {
        // A budget of one segment forces a merge after every flush, which is
        // the point of this case: the merge has to happen before the restart.
        auto engine = OpenFixture(fixture, PolicyOptions(8, 1, true, 0));
        if (!engine) return;
        for (std::uint32_t index = 0; index < 24; ++index) {
            const auto write = engine->Insert(960000 + index, FarVector(fixture, index),
                                              WriteOptions{});
            Require(std::holds_alternative<WriteReceipt>(write), "merge-restart fixture write");
        }
        const bool merged = WaitFor(
            [&] {
                const auto stats = engine->Stats();
                return std::holds_alternative<EngineStats>(stats) &&
                       std::get<EngineStats>(stats).merge_bytes_written > 0;
            },
            std::chrono::seconds(30));
        Require(merged, "the merge runs before the restart");
        const bool quiet = WaitFor(
            [&] {
                const auto stats = engine->Stats();
                return std::holds_alternative<EngineStats>(stats) &&
                       std::get<EngineStats>(stats).merge_backlog_bytes == 0;
            },
            std::chrono::seconds(30));
        Require(quiet, "the merge leaves nothing unflushed");
        // Reclamation is part of the same publish: the merged segment replaces
        // the base and every flushed delta, so exactly one directory may remain.
        const bool reclaimed = WaitFor(
            [&] { return SegmentDirectories(fixture) == 1; }, std::chrono::seconds(30));
        Require(reclaimed, "a merge reclaims every superseded segment directory");
        std::error_code scan_error;
        for (const auto& entry : std::filesystem::directory_iterator(fixture.index, scan_error)) {
            if (scan_error) break;
            const auto name = entry.path().filename().string();
            if (name.find("checkpoint-") == 0 || name.find("merge-") == 0)
                Require(false, "no staging directory is left behind");
        }
        const auto stats = engine->Stats();
        if (std::holds_alternative<EngineStats>(stats)) closed_stats = std::get<EngineStats>(stats);
        closed_directories = SegmentDirectories(fixture);
        static_cast<void>(engine->Close());
    }
    Require(closed_directories == 1, "the closed index still holds one segment directory");

    {
        auto engine = OpenFixture(fixture, Options(8u << 20, false));
        if (!engine) return;
        const auto stats = engine->Stats();
        if (std::holds_alternative<EngineStats>(stats)) {
            const auto& value = std::get<EngineStats>(stats);
            Require(value.published_sequence == 24, "restart keeps the merged sequence");
            Require(value.checkpoint_sequence == 24, "restart sees the merged segment's coverage");
            Require(value.live_vectors == fixture.count + 24, "restart keeps the merged live count");
            Require(value.live_vectors == closed_stats.live_vectors,
                    "the restart agrees with the engine that merged");
        }
        std::uint32_t readable = 0;
        for (std::uint32_t index = 0; index < 24; ++index)
            if (std::holds_alternative<StoredVector>(engine->Get(960000 + index))) ++readable;
        Require(readable == 24, "every row a merge rewrote is readable after a restart");
        // The base rows are the ones a merge could silently drop.
        std::uint32_t base_readable = 0;
        for (std::uint32_t index = 0; index < 40; ++index)
            if (std::holds_alternative<StoredVector>(engine->Get(fixture.identifiers[index])))
                ++base_readable;
        Require(base_readable == 40, "the merged segment keeps the rows it inherited");
        const auto next = engine->Insert(960100, FarVector(fixture, 30), WriteOptions{});
        Require(std::holds_alternative<WriteReceipt>(next),
                "writes continue after a merge and a restart");
        if (std::holds_alternative<WriteReceipt>(next))
            Require(std::get<WriteReceipt>(next).sequence == 25,
                    "the sequence continues from the merged checkpoint");
        static_cast<void>(engine->Close());
    }
}

// A generation switch replaces the adapter under readers. Queries running while
// maintenance publishes must never fail and never see a different answer.
void TestQueriesAnswerAcrossGenerationSwitches() {
    auto fixture = Prepare("switch");
    auto engine = OpenFixture(fixture, Options(8u << 20, true));
    if (!engine) return;

    std::vector<std::vector<float>> queries;
    for (std::uint32_t index = 0; index < 4; ++index)
        queries.push_back(VectorFor(fixture, index * 71 + 3));
    std::vector<std::vector<VectorId>> expected;
    for (const auto& query : queries) expected.push_back(QueryIds(*engine, query, 10));
    Require(!expected.empty() && !expected.front().empty(), "the fixture answers before switching");

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> answered{0};
    std::atomic<std::uint32_t> failures{0};
    std::atomic<std::uint32_t> drifted{0};
    std::thread reader([&] {
        while (!stop.load()) {
            for (std::size_t index = 0; index < queries.size(); ++index) {
                SearchOptions options;
                options.k = 10;
                options.candidate_limit = 64;
                const auto found = engine->Search(queries[index], options);
                if (const auto* error = std::get_if<Error>(&found)) {
                    static_cast<void>(error);
                    ++failures;
                    continue;
                }
                std::vector<VectorId> ids;
                for (const auto& neighbour : std::get<SearchResult>(found).neighbors)
                    ids.push_back(neighbour.id);
                if (ids != expected[index]) ++drifted;
                ++answered;
            }
        }
    });

    for (std::uint32_t round = 0; round < 10; ++round) {
        for (std::uint32_t index = 0; index < 10; ++index) {
            const auto write = engine->Insert(970000 + round * 10 + index,
                                              FarVector(fixture, round * 10 + index),
                                              WriteOptions{});
            Require(std::holds_alternative<WriteReceipt>(write), "switch fixture write");
        }
        const Sequence expected_sequence = (round + 1) * 10;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto stats = engine->Stats();
        if (std::holds_alternative<EngineStats>(stats))
            Require(std::get<EngineStats>(stats).published_sequence == expected_sequence,
                    "every acknowledged write advances the published sequence");
    }
    const bool quiet = WaitFor(
        [&] {
            const auto stats = engine->Stats();
            return std::holds_alternative<EngineStats>(stats) &&
                   std::get<EngineStats>(stats).merge_backlog_bytes == 0;
        },
        std::chrono::seconds(30));
    Require(quiet, "the switch fixture drains its delta");
    stop.store(true);
    reader.join();

    Require(answered.load() > 0, "queries kept running across the switches");
    Require(failures.load() == 0, "no query failed while a generation was switching");
    Require(drifted.load() == 0, "no answer moved while a generation was switching");
    {
        const auto stats = engine->Stats();
        const auto generation = std::holds_alternative<EngineStats>(stats)
                                    ? std::get<EngineStats>(stats).view_generation
                                    : 0;
        std::printf("switch: answered=%llu failures=%llu drifted=%llu generations=%llu\n",
                    static_cast<unsigned long long>(answered.load()),
                    static_cast<unsigned long long>(failures.load()),
                    static_cast<unsigned long long>(drifted.load()),
                    static_cast<unsigned long long>(generation));
    }

    // The published manifest, not the in-memory copy, is what a restart sees.
    const auto described = DescribeIndex(fixture.index);
    Require(std::holds_alternative<IndexDescription>(described), "the published index describes");
    if (std::holds_alternative<IndexDescription>(described)) {
        const auto& value = std::get<IndexDescription>(described);
        Require(value.checkpoint_sequence == 100, "CURRENT names the generation that covers 100");
        Require(value.segment_count >= 1, "the published generation has a segment");
        Require(value.live_count == fixture.count + 100, "the published live count matches");
        const auto stats = engine->Stats();
        if (std::holds_alternative<EngineStats>(stats))
            Require(value.generation == std::get<EngineStats>(stats).view_generation,
                    "the engine serves the generation CURRENT names");
    }
    static_cast<void>(engine->Close());
}

// --- multi-round load and policy ablation ---------------------------------

struct PolicyResult {
    std::string name;
    std::uint32_t writes = 0;
    std::uint32_t unreadable = 0;
    std::uint64_t checkpoint_bytes = 0;
    std::uint64_t merge_bytes = 0;
    std::uint64_t peak_backlog = 0;
    std::uint32_t peak_directories = 0;
    std::uint32_t final_directories = 0;
    // Rounds whose delta never drained inside the wait: zero for a policy that
    // keeps up, non-zero is recorded rather than hidden.
    std::uint32_t settled = 0;
    // Set to zero when a policy that is supposed to hold the segment budget
    // needed the wait and still did not get there.
    std::uint32_t budget_ok = 1;
    long long elapsed_ms = 0;
};

// Same data, same rounds, same budget: only the maintenance policy differs.
// Each round waits for the delta it produced to be drained, so the outcome is
// decided by the policy and not by how fast this machine happens to be. The
// elapsed time of every round is still reported: it is the write cost each arm
// pays for its policy.
PolicyResult RunPolicy(const std::string& name, const EngineOptions& options,
                       std::uint32_t rounds, bool drains, bool holds_budget) {
    PolicyResult result;
    result.name = name;
    auto fixture = Prepare("policy-" + name);
    auto engine = OpenFixture(fixture, options);
    if (!engine) return result;

    const auto started = std::chrono::steady_clock::now();
    for (std::uint32_t round = 0; round < rounds; ++round) {
        for (std::uint32_t index = 0; index < 10; ++index) {
            const auto id = 980000 + round * 10 + index;
            const auto write = engine->Insert(id, FarVector(fixture, round * 10 + index),
                                              WriteOptions{});
            if (std::holds_alternative<WriteReceipt>(write)) ++result.writes;
        }
        const auto stats = engine->Stats();
        if (std::holds_alternative<EngineStats>(stats)) {
            const auto& value = std::get<EngineStats>(stats);
            result.peak_backlog = (std::max)(result.peak_backlog, value.merge_backlog_bytes);
        }
        result.peak_directories = (std::max)(result.peak_directories, SegmentDirectories(fixture));
        if (!drains) continue;
        // Wait until this round is on disk as a segment before writing the next
        // one, so the segment count and the merge work are the same every run.
        const bool quiet = WaitFor(
            [&] {
                const auto current = engine->Stats();
                return std::holds_alternative<EngineStats>(current) &&
                       std::get<EngineStats>(current).merge_backlog_bytes == 0;
            },
            std::chrono::seconds(20));
        if (!quiet) ++result.settled;
    }
    if (holds_budget) {
        // The last round may have left the segment count one over budget: the
        // merge that fixes it is the next thing maintenance does.
        const bool bounded = WaitFor(
            [&] { return SegmentDirectories(fixture) <= 3; }, std::chrono::seconds(20));
        if (!bounded) result.budget_ok = 0;
    }
    result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started).count();
    const auto stats = engine->Stats();
    if (std::holds_alternative<EngineStats>(stats)) {
        const auto& value = std::get<EngineStats>(stats);
        result.checkpoint_bytes = value.checkpoint_bytes_written;
        result.merge_bytes = value.merge_bytes_written;
        result.peak_backlog = (std::max)(result.peak_backlog, value.merge_backlog_bytes);
    }
    for (std::uint32_t index = 0; index < result.writes; ++index)
        if (!std::holds_alternative<StoredVector>(engine->Get(980000 + index)))
            ++result.unreadable;
    result.final_directories = SegmentDirectories(fixture);
    static_cast<void>(engine->Close());
    return result;
}

void TestMaintenancePolicyAblation() {
    const std::uint32_t rounds = 8;
    const std::uint32_t writes = rounds * 10;
    // One delta record at dimension 8: the vector, its id and sequence, and
    // the per-slot bookkeeping the delta layer charges.
    const std::uint64_t record_bytes = DeltaRecordBytes(8);
    std::vector<PolicyResult> results;
    // The fixed control: no maintenance at all, so every write stays in the
    // delta and the backlog is the whole workload.
    results.push_back(RunPolicy("off", Options(8u << 20, false), rounds, false, false));
    // Flush only: the segment count grows without bound; the only rule that
    // keeps it in check is the merge.
    results.push_back(RunPolicy("flush", PolicyOptions(8, 15, true, 0), rounds, true, false));
    // The shipped policy.
    results.push_back(RunPolicy("flush+merge", PolicyOptions(8, 2, true, 0), rounds, true, true));
    // Ablation: a minimum merge size larger than the whole data set, so every
    // merge is refused and the segment budget stops being enforced.
    results.push_back(RunPolicy("min-6MiB", PolicyOptions(8, 2, true, 6u << 20), rounds, true,
                                false));

    for (const auto& result : results) {
        std::printf("policy %-12s writes=%u unreadable=%u ckpt_bytes=%llu merge_bytes=%llu "
                    "peak_backlog=%llu peak_dirs=%u final_dirs=%u undrained=%u budget=%u "
                    "elapsed_ms=%lld\n",
                    result.name.c_str(), result.writes, result.unreadable,
                    static_cast<unsigned long long>(result.checkpoint_bytes),
                    static_cast<unsigned long long>(result.merge_bytes),
                    static_cast<unsigned long long>(result.peak_backlog),
                    result.peak_directories, result.final_directories, result.settled,
                    result.budget_ok, result.elapsed_ms);
    }

    const auto& off = results[0];
    const auto& flush = results[1];
    const auto& shipped = results[2];
    const auto& minimum = results[3];

    for (const auto& result : results) {
        Require(result.writes == writes, "every write in the load is acknowledged");
        Require(result.unreadable == 0, "every write in the load is readable afterwards");
    }
    // Without maintenance the backlog is the whole workload: that is the
    // reference the other arms are measured against, not a passing state.
    Require(off.peak_backlog >= static_cast<std::uint64_t>(writes) * record_bytes,
            "the fixed control keeps the whole workload in the delta");
    Require(off.merge_bytes == 0 && off.checkpoint_bytes == 0,
            "the fixed control never writes a segment");
    // Flush only: bounded backlog, unbounded segment count.
    Require(flush.peak_directories > 3, "flush-only lets the segment count grow");
    Require(flush.final_directories > 3, "flush-only never reclaims");
    Require(flush.merge_bytes == 0, "flush-only never merges");
    // The shipped policy holds both bounds.
    Require(shipped.settled == 0, "the shipped policy drains every round inside the wait");
    Require(shipped.budget_ok == 1, "the shipped policy returns to its segment budget");
    Require(shipped.final_directories <= 3, "the shipped policy keeps the segment count bounded");
    // Transient bound: the segment budget, plus the flush that may land between
    // the sample and the merge, plus the merged directory staged before the
    // superseded ones are released.
    Require(shipped.peak_directories <= 5, "the shipped policy bounds the transient segment count");
    Require(shipped.merge_bytes > 0, "the shipped policy measured merge work");
    // One round of ten writes is the most the delta may hold when the policy
    // keeps up; the fixed control holds all of them.
    Require(shipped.peak_backlog <= static_cast<std::uint64_t>(11) * record_bytes,
            "the shipped policy bounds the backlog to a single round");
    Require(shipped.peak_backlog < off.peak_backlog,
            "the shipped policy holds less backlog than the fixed control");
    // The ablation has to differ, otherwise it measures nothing.
    Require(minimum.merge_bytes == 0, "a minimum merge size above the data skips every merge");
    Require(minimum.final_directories > 3,
            "a skipped merge leaves the segments where the budget stops applying");
    Require(minimum.peak_directories > 3, "a skipped merge is visible in the segment count");
}

// A rate is not a batch. Every arm above drains its round before writing the
// next one, which says the policy is cheap but nothing about keeping up. This
// arm writes on a fixed cadence, never waits for the delta, and queries at the
// same time: a policy that falls behind shows up as a backlog that grows with
// the run instead of staying at one trigger.
struct RateResult {
    std::string name;
    std::uint32_t rate = 0;
    long long think_ms = 0;
    std::uint32_t attempted = 0;
    std::uint32_t accepted = 0;
    std::uint32_t refused = 0;
    std::uint32_t unreadable = 0;
    std::uint64_t queries = 0;
    std::uint32_t query_failures = 0;
    std::uint32_t undrained = 0;
    // Peak backlog over the first and the last third of the stream: a policy
    // that keeps up has the same peak in both, one that does not grows.
    std::uint64_t early_peak = 0;
    std::uint64_t late_peak = 0;
    std::uint64_t peak_backlog = 0;
    std::uint32_t peak_directories = 0;
    std::uint32_t final_directories = 0;
    // Set to zero when the segment count did not return to the budget after the
    // stream stopped.
    std::uint32_t budget_ok = 1;
    long long elapsed_ms = 0;
    // Time from the last write to an empty backlog: what the stream leaves
    // behind for the next quiet moment to clean up.
    long long drain_ms = 0;
    // Writes actually completed per second: below the declared rate the arm
    // could not hold the cadence, which is itself the result for that arm.
    double achieved = 0.0;
    std::string first_error;
};

RateResult RunAtRate(const std::string& name, const EngineOptions& options, std::uint32_t rate,
                     std::chrono::seconds duration, bool expect_drain,
                     std::chrono::milliseconds think_time) {
    RateResult result;
    result.name = name;
    result.rate = rate;
    result.think_ms = think_time.count();
    auto fixture = Prepare("rate-" + name);
    auto engine = OpenFixture(fixture, options);
    if (!engine) return result;

    const auto started = std::chrono::steady_clock::now();
    const auto length = std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> queries{0};
    std::atomic<std::uint32_t> query_failures{0};
    const auto probe = VectorFor(fixture, 7);
    std::thread reader([&] {
        while (!stop.load()) {
            SearchOptions search;
            search.k = 10;
            search.candidate_limit = 64;
            const auto found = engine->Search(probe, search);
            if (std::holds_alternative<Error>(found)) {
                ++query_failures;
                continue;
            }
            ++queries;
            // A zero think time means the reader reissues the instant it gets an
            // answer, which is the worst case for the writer.
            if (think_time.count() > 0) std::this_thread::sleep_for(think_time);
        }
    });

    const std::uint32_t total = rate * static_cast<std::uint32_t>(duration.count());
    std::vector<VectorId> accepted;
    accepted.reserve(total);
    for (std::uint32_t index = 0; index < total; ++index) {
        const auto used = std::chrono::steady_clock::now() - started;
        if (used >= length) break;
        // Pace against an absolute deadline. Sleeping a fixed interval instead
        // would let a slow write shift the rest of the stream, so the arm would
        // measure its own drift rather than the declared rate.
        const auto deadline = length * index / total;
        if (used < deadline) std::this_thread::sleep_for(deadline - used);
        ++result.attempted;
        const VectorId id = 910000 + index;
        const auto write = engine->Insert(id, FarVector(fixture, index), WriteOptions{});
        if (const auto* error = std::get_if<Error>(&write)) {
            ++result.refused;
            if (result.first_error.empty()) result.first_error = error->message;
        } else {
            ++result.accepted;
            accepted.push_back(id);
        }
        if (index % 8 != 7) continue;
        const auto stats = engine->Stats();
        if (!std::holds_alternative<EngineStats>(stats)) continue;
        const auto backlog = std::get<EngineStats>(stats).merge_backlog_bytes;
        result.peak_backlog = (std::max)(result.peak_backlog, backlog);
        // Bucket by wall clock, not by write index: an arm that cannot keep the
        // cadence gets fewer writes in the same time, and comparing index
        // buckets would then compare two different amounts of elapsed work.
        const auto elapsed = std::chrono::steady_clock::now() - started;
        if (elapsed * 3 < length)
            result.early_peak = (std::max)(result.early_peak, backlog);
        else if (elapsed * 3 > length * 2)
            result.late_peak = (std::max)(result.late_peak, backlog);
        result.peak_directories = (std::max)(result.peak_directories, SegmentDirectories(fixture));
    }
    result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started).count();
    result.achieved =
        static_cast<double>(result.attempted) * 1000.0 / static_cast<double>(result.elapsed_ms);
    stop.store(true);
    reader.join();

    // Only a policy that flushes has anything to drain; the control would just
    // spend the whole timeout confirming that nothing happens.
    bool quiet = false;
    if (expect_drain)
        quiet = WaitFor(
            [&] {
                const auto stats = engine->Stats();
                return std::holds_alternative<EngineStats>(stats) &&
                       std::get<EngineStats>(stats).merge_backlog_bytes == 0;
            },
            std::chrono::seconds(30));
    result.drain_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - started).count() -
                      result.elapsed_ms;
    if (!quiet) ++result.undrained;
    for (const auto id : accepted)
        if (!std::holds_alternative<StoredVector>(engine->Get(id))) ++result.unreadable;
    // An empty backlog is not the same moment as a restored segment budget: the
    // flush that empties the delta may have left one segment too many, and the
    // merge that fixes it is the next thing maintenance does.
    if (expect_drain) {
        const bool bounded = WaitFor([&] { return SegmentDirectories(fixture) <= 3; },
                                     std::chrono::seconds(20));
        if (!bounded) result.budget_ok = 0;
    }
    result.final_directories = SegmentDirectories(fixture);
    result.queries = queries.load();
    result.query_failures = query_failures.load();
    static_cast<void>(engine->Close());
    return result;
}

// The declared rate is the one the delivered policy holds with a bounded
// backlog and no refused write. The higher arms are reported, not asserted: they
// show where the full-rewrite merge stops keeping up, which is the honest
// ceiling of this implementation rather than a number to hide.
void TestSustainedWriteRate() {
    const std::uint64_t record_bytes = DeltaRecordBytes(8);
    const std::uint32_t declared = 200;
    const auto think = std::chrono::milliseconds(1);
    std::vector<RateResult> results;
    for (const std::uint32_t rate : {50u, 100u, 200u, 400u})
        results.push_back(RunAtRate(std::to_string(rate), PolicyOptions(8, 2, true, 0), rate,
                                    std::chrono::seconds(3), true, think));
    // Same declared rate with maintenance off: the backlog is then the whole
    // stream, which is what "bounded" is being contrasted with.
    results.push_back(RunAtRate("control", Options(8u << 20, false), declared,
                                std::chrono::seconds(3), false, think));
    // A reader that never pauses: the worst case for a writer that needs the
    // same lock. Reported rather than asserted, because the point is to show the
    // cost of no think time, not to claim the stream holds its rate.
    results.push_back(RunAtRate("declared-tight", PolicyOptions(8, 2, true, 0), declared,
                                std::chrono::seconds(2), true, std::chrono::milliseconds(0)));
    const RateResult& tight = results.back();

    for (const auto& result : results)
        std::printf("rate %-8s declared=%u/s achieved=%.0f/s think_ms=%lld attempted=%u "
                    "accepted=%u refused=%u unreadable=%u queries=%llu qfail=%u early_peak=%llu "
                    "late_peak=%llu peak_dirs=%u final_dirs=%u undrained=%u budget=%u "
                    "elapsed_ms=%lld drain_ms=%lld%s%s\n",
                    result.name.c_str(), result.rate, result.achieved, result.think_ms,
                    result.attempted, result.accepted, result.refused, result.unreadable,
                    static_cast<unsigned long long>(result.queries), result.query_failures,
                    static_cast<unsigned long long>(result.early_peak),
                    static_cast<unsigned long long>(result.late_peak), result.peak_directories,
                    result.final_directories, result.undrained, result.budget_ok, result.elapsed_ms,
                    result.drain_ms, result.first_error.empty() ? "" : " err=",
                    result.first_error.c_str());

    const RateResult* shipped = nullptr;
    for (const auto& result : results)
        if (result.rate == declared && result.name != "control" && result.name != "declared-tight")
            shipped = &result;
    const auto& control = results[results.size() - 2];
    Require(shipped != nullptr, "the declared rate was measured");
    if (shipped == nullptr) return;

    for (const auto& result : results) {
        Require(result.query_failures == 0, "queries answered while the stream was running");
        Require(result.unreadable == 0, "every acknowledged write reads back after the stream");
    }
    // A declared sustainable rate means what it says: nothing was refused and
    // the stream kept its cadence to within a tenth.
    Require(shipped->refused == 0, "the declared rate accepts every write");
    Require(shipped->attempted >= declared * 3 * 9 / 10,
            "the declared rate holds its cadence for the whole stream");
    // Keeping up is an absolute bound, not a trend: the policy drains as it goes,
    // so the backlog never holds more than a second of writes at this rate. The
    // early and late peaks are printed for the record, but sampled every eighth
    // write they alias against the flush period, so they are not the test.
    Require(shipped->peak_backlog <= static_cast<std::uint64_t>(declared) * record_bytes,
            "the backlog stays inside a second of writes at the declared rate");
    Require(shipped->peak_directories <= 5, "the segment count stays inside the budget");
    Require(shipped->budget_ok == 1, "the segment count returns to the budget");
    Require(shipped->final_directories <= 3, "the segment count ends inside the budget");
    Require(shipped->undrained == 0, "the stream drains once the writes stop");
    // The contrast: without maintenance the same rate keeps everything. The
    // tolerance is one sampling gap, since the backlog is sampled every eighth
    // write rather than after the last one.
    Require(control.peak_backlog >=
                (static_cast<std::uint64_t>(control.accepted) - 8) * record_bytes,
            "the fixed control keeps the whole stream in the delta");
    Require(shipped->peak_backlog < control.peak_backlog / 2,
            "the shipped policy holds far less than the stream at the same rate");
    // The cost of a reader that never pauses: same declared rate, same policy,
    // only the read side differs. Recorded, not asserted.
    Require(tight.attempted > 0, "the arm without think time produced writes");
    std::printf("read pressure: think=1ms achieved=%.0f/s | think=0 achieved=%.0f/s\n",
                shipped->achieved, tight.achieved);
}

// The settle rule is the one adaptive element of the shipped policy: a write
// stream that pauses gets flushed even when it never reached the trigger. This
// is the arms-on/arms-off comparison for it, on a workload small enough to be
// decided by that rule alone.
void TestSettleRuleIsAblatable() {
    const EngineOptions on = PolicyOptions(8, 15, true, 0);
    const EngineOptions off = PolicyOptions(8, 15, false, 0);
    EngineStats with_rule{};
    EngineStats without_rule{};
    {
        auto fixture = Prepare("settle-on");
        auto engine = OpenFixture(fixture, on);
        if (!engine) return;
        for (std::uint32_t index = 0; index < 3; ++index)
            Require(std::holds_alternative<WriteReceipt>(
                        engine->Insert(990000 + index, FarVector(fixture, index), WriteOptions{})),
                    "settle fixture write");
        // Three records never reach a trigger of eight; only the pause rule can
        // turn them into a segment.
        const bool flushed = WaitFor(
            [&] {
                const auto stats = engine->Stats();
                return std::holds_alternative<EngineStats>(stats) &&
                       std::get<EngineStats>(stats).checkpoint_sequence == 3;
            },
            std::chrono::seconds(20));
        Require(flushed, "the settle rule flushes a paused stream below the trigger");
        const auto stats = engine->Stats();
        if (std::holds_alternative<EngineStats>(stats)) with_rule = std::get<EngineStats>(stats);
        static_cast<void>(engine->Close());
    }
    {
        auto fixture = Prepare("settle-off");
        auto engine = OpenFixture(fixture, off);
        if (!engine) return;
        for (std::uint32_t index = 0; index < 3; ++index)
            Require(std::holds_alternative<WriteReceipt>(
                        engine->Insert(990000 + index, FarVector(fixture, index), WriteOptions{})),
                    "settle fixture write");
        // Give the loop the same wall clock as the arm above before deciding.
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto stats = engine->Stats();
        if (std::holds_alternative<EngineStats>(stats)) without_rule = std::get<EngineStats>(stats);
        static_cast<void>(engine->Close());
    }
    Require(with_rule.checkpoint_sequence == 3, "with the rule the records reach a segment");
    Require(without_rule.checkpoint_sequence == 0, "without the rule they stay in the delta");
    Require(without_rule.merge_backlog_bytes > 0, "without the rule the backlog is still open");
    Require(with_rule.merge_backlog_bytes == 0, "with the rule the backlog is closed");
    std::printf("settle-rule on: ckpt=%llu backlog=%llu | off: ckpt=%llu backlog=%llu\n",
                static_cast<unsigned long long>(with_rule.checkpoint_sequence),
                static_cast<unsigned long long>(with_rule.merge_backlog_bytes),
                static_cast<unsigned long long>(without_rule.checkpoint_sequence),
                static_cast<unsigned long long>(without_rule.merge_backlog_bytes));
}

}  // namespace

int main() {
    // Per-case counts are reported so a reviewer can tell which case a change
    // in the total belongs to.
    const auto run = [](const char* name, void (*body)()) {
        const int before = g_checks;
        const int failed_before = g_failures;
        body();
        std::printf("case %-46s checks=%d failures=%d\n", name, g_checks - before,
                    g_failures - failed_before);
    };
    run("CheckpointPublishesWithoutChangingAnswers",
        &TestCheckpointPublishesWithoutChangingAnswers);
    run("RestartAfterCheckpointDoesNotReplay", &TestRestartAfterCheckpointDoesNotReplay);
    run("MergesKeepSegmentsBounded", &TestMergesKeepSegmentsBounded);
    run("MergeSurvivesRestart", &TestMergeSurvivesRestart);
    run("QueryResultsSurviveMerge", &TestQueryResultsSurviveMerge);
    run("QueriesAnswerAcrossGenerationSwitches", &TestQueriesAnswerAcrossGenerationSwitches);
    run("BackgroundMaintenanceFlushesWithoutACall",
        &TestBackgroundMaintenanceFlushesWithoutACall);
    run("MaintenancePolicyAblation", &TestMaintenancePolicyAblation);
    run("SustainedWriteRate", &TestSustainedWriteRate);
    run("SettleRuleIsAblatable", &TestSettleRuleIsAblatable);

    std::printf("maintenance: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d maintenance checks failed\n", g_failures);
        return 1;
    }
    return 0;
}
