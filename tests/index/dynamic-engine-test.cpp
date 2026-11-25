// Stage 6 B checks: durable writes, snapshot visibility, recovery and the
// refusal paths that must not leave a half-applied record behind.
//
// The acceptance condition is narrow on purpose: an acknowledged write is
// visible to the next query, a newer version hides the older one, and a restart
// replays exactly the writes that were acknowledged -- no more, no fewer.

#include "agent_io/engine.hpp"
#include "agent_io/index.hpp"
#include "agent_io/storage.hpp"

#include "../../src/index/builder.hpp"
#include "../../src/index/format.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
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

double Distance(const float* left, const float* right, std::uint32_t dimension) {
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
    std::uint32_t count = 192;
};

std::uint64_t g_serial = 0;

Fixture Prepare(const std::string& name) {
    Fixture fixture;
    fixture.root = std::filesystem::temp_directory_path() /
        ("agent_io_dynamic_" + name + "_" + std::to_string(GetCurrentProcessId()) + "_" +
         std::to_string(++g_serial));
    std::error_code error;
    std::filesystem::remove_all(fixture.root, error);
    std::filesystem::create_directories(fixture.root, error);
    fixture.index = fixture.root / "index";
    fixture.staging = fixture.root / "index.staging";

    const std::uint32_t clusters = 12;
    std::uint64_t seed = 7;
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
    request.build_id = "dynamic-engine-test";
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

EngineOptions Options(std::uint64_t delta_bytes = 8u << 20) {
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

void TestConfirmThenVisible() {
    auto fixture = Prepare("visible");
    auto engine = OpenFixture(fixture, Options());
    if (!engine) return;

    const VectorId fresh = 900001;
    std::vector<float> values = VectorFor(fixture, 5);
    const auto receipt = engine->Insert(fresh, values, WriteOptions{});
    Require(std::holds_alternative<WriteReceipt>(receipt), "insert is acknowledged");
    if (std::holds_alternative<WriteReceipt>(receipt))
        Require(std::get<WriteReceipt>(receipt).sequence == 1, "the first write is sequence 1");

    // The acknowledgement must already be visible: no flush, no reopen.
    const auto stored = engine->Get(fresh);
    Require(std::holds_alternative<StoredVector>(stored), "a confirmed write is readable");
    if (std::holds_alternative<StoredVector>(stored)) {
        const auto& vector = std::get<StoredVector>(stored);
        Require(vector.id == fresh && vector.version == 1, "the stored version is the write");
        Require(vector.values.size() == values.size() &&
                std::equal(vector.values.begin(), vector.values.end(), values.begin()),
                "the stored vector is the written one");
    }

    // A query for the same vector must return the fresh ID among its results.
    // The fixture already holds an identical point, so the write ties on
    // distance rather than winning outright; what matters is that the delta is
    // re-ranked at all, without waiting for a checkpoint.
    SearchOptions search;
    search.k = 5;
    search.candidate_limit = 64;
    const auto found = engine->Search(values, search);
    Require(std::holds_alternative<SearchResult>(found), "search after a write");
    if (std::holds_alternative<SearchResult>(found)) {
        const auto& result = std::get<SearchResult>(found);
        Require(result.snapshot_sequence == 1, "the query snapshot is the published sequence");
        bool present = false;
        for (const auto& neighbour : result.neighbors)
            if (neighbour.id == fresh) {
                present = true;
                Require(neighbour.version == 1, "the visible version is the new write");
                Require(neighbour.squared_distance == 0.0,
                        "the delta record is scored on its own vector");
            }
        Require(present, "the confirmed write participates in the next query");
        Require(result.metrics.delta_records_scanned == 1, "the delta reports one scanned record");
    }

    const auto stats = engine->Stats();
    if (std::holds_alternative<EngineStats>(stats)) {
        const auto& counters = std::get<EngineStats>(stats);
        Require(counters.published_sequence == 1, "stats publish the sequence");
        Require(counters.live_vectors == fixture.count + 1, "the live count grows once");
        Require(counters.wal_bytes_written > 0, "the WAL recorded bytes");
        Require(counters.pending_write_count == 0, "no write stays pending");
    }
}

void TestVersionsAndConflict() {
    auto fixture = Prepare("versions");
    auto engine = OpenFixture(fixture, Options());
    if (!engine) return;

    const VectorId id = fixture.identifiers[4];
    std::vector<float> first = VectorFor(fixture, 4);
    const auto duplicate = engine->Insert(id, first, WriteOptions{});
    Require(std::holds_alternative<Error>(duplicate) &&
            std::get<Error>(duplicate).code == ErrorCode::already_exists,
            "inserting a visible ID is a conflict");

    std::vector<float> replacement(fixture.dimension);
    for (std::uint32_t component = 0; component < fixture.dimension; ++component)
        replacement[component] = first[component] + 0.25f;
    const auto upsert = engine->Upsert(id, replacement, WriteOptions{});
    Require(std::holds_alternative<WriteReceipt>(upsert), "upsert is acknowledged");
    if (std::holds_alternative<WriteReceipt>(upsert))
        Require(std::get<WriteReceipt>(upsert).sequence == 1, "the first write takes sequence 1");

    const auto stored = engine->Get(id);
    Require(std::holds_alternative<StoredVector>(stored), "the upserted ID is readable");
    if (std::holds_alternative<StoredVector>(stored)) {
        const auto& vector = std::get<StoredVector>(stored);
        Require(vector.version == 1, "the newer version is the visible one");
        Require(std::equal(vector.values.begin(), vector.values.end(), replacement.begin()),
                "the newer values replaced the older ones");
    }

    // A second upsert must win again, and the older version must not resurface
    // through a query that ranks the delta.
    std::vector<float> newest = replacement;
    newest[0] += 0.5f;
    const auto second = engine->Upsert(id, newest, WriteOptions{});
    Require(std::holds_alternative<WriteReceipt>(second), "a second upsert is acknowledged");
    if (std::holds_alternative<WriteReceipt>(second))
        Require(std::get<WriteReceipt>(second).sequence == 2, "sequences keep increasing");

    SearchOptions search;
    search.k = 4;
    search.candidate_limit = 64;
    const auto found = engine->Search(newest, search);
    Require(std::holds_alternative<SearchResult>(found), "search after two upserts");
    if (std::holds_alternative<SearchResult>(found)) {
        const auto& result = std::get<SearchResult>(found);
        std::size_t occurrences = 0;
        for (const auto& neighbour : result.neighbors) {
            if (neighbour.id != id) continue;
            ++occurrences;
            Require(neighbour.version == 2, "a query reports the newest version only");
        }
        Require(occurrences == 1, "the same ID appears at most once in a result");
    }
}

void TestSnapshotIsolation() {
    auto fixture = Prepare("snapshot");
    auto engine = OpenFixture(fixture, Options());
    if (!engine) return;

    SearchOptions search;
    search.k = 6;
    search.candidate_limit = 64;
    const auto before = engine->Search(VectorFor(fixture, 11), search);
    Require(std::holds_alternative<SearchResult>(before), "the first query succeeds");
    const Sequence snapshot_before =
        std::holds_alternative<SearchResult>(before) ?
            std::get<SearchResult>(before).snapshot_sequence : 0;

    const VectorId fresh = 910001;
    const auto written = engine->Insert(fresh, VectorFor(fixture, 11), WriteOptions{});
    Require(std::holds_alternative<WriteReceipt>(written), "a write between queries");

    const auto after = engine->Search(VectorFor(fixture, 11), search);
    Require(std::holds_alternative<SearchResult>(after), "the second query succeeds");
    if (std::holds_alternative<SearchResult>(after) &&
        std::holds_alternative<SearchResult>(before)) {
        const auto& later = std::get<SearchResult>(after);
        Require(later.snapshot_sequence > snapshot_before, "the new query sees a newer snapshot");
        bool present = false;
        for (const auto& neighbour : later.neighbors)
            if (neighbour.id == fresh) present = true;
        Require(present, "the write participates in the next query");
        for (const auto& neighbour : std::get<SearchResult>(before).neighbors)
            Require(neighbour.id != fresh, "the earlier result cannot contain the later write");
    }
}

void TestRecoveryReplaysConfirmedWrites() {
    auto fixture = Prepare("recovery");
    std::vector<VectorId> written;
    Sequence last_sequence = 0;
    {
        auto engine = OpenFixture(fixture, Options());
        if (!engine) return;
        for (std::uint32_t index = 0; index < 5; ++index) {
            const VectorId id = 920000 + index;
            const auto receipt = engine->Insert(id, VectorFor(fixture, index), WriteOptions{});
            Require(std::holds_alternative<WriteReceipt>(receipt), "recovery fixture write");
            if (std::holds_alternative<WriteReceipt>(receipt)) {
                last_sequence = std::get<WriteReceipt>(receipt).sequence;
                written.push_back(id);
            }
        }
        const auto closed = engine->Close();
        Require(AsError(closed) == nullptr, "the engine closes cleanly");
    }
    Require(!WalFiles(fixture).empty(), "a WAL file exists after writes");

    {
        auto engine = OpenFixture(fixture, Options());
        if (!engine) return;
        const auto stats = engine->Stats();
        if (std::holds_alternative<EngineStats>(stats)) {
            Require(std::get<EngineStats>(stats).published_sequence == last_sequence,
                    "recovery restores the published sequence");
            Require(std::get<EngineStats>(stats).live_vectors == fixture.count + written.size(),
                    "recovery restores the live count");
        }
        for (std::size_t index = 0; index < written.size(); ++index) {
            const auto stored = engine->Get(written[index]);
            Require(std::holds_alternative<StoredVector>(stored),
                    "every confirmed write survives a restart");
            if (std::holds_alternative<StoredVector>(stored))
                Require(std::get<StoredVector>(stored).version == index + 1,
                        "the replayed version matches the original sequence");
        }
        // The engine must be able to keep writing at the next sequence, not
        // restart from one and reuse versions.
        const auto next = engine->Insert(929999, VectorFor(fixture, 9), WriteOptions{});
        Require(std::holds_alternative<WriteReceipt>(next), "writes continue after recovery");
        if (std::holds_alternative<WriteReceipt>(next))
            Require(std::get<WriteReceipt>(next).sequence == last_sequence + 1,
                    "the sequence continues instead of restarting");
        static_cast<void>(engine->Close());
    }

    // Opening twice must not replay twice: the second open sees the same state.
    {
        auto engine = OpenFixture(fixture, Options());
        if (!engine) return;
        const auto stats = engine->Stats();
        if (std::holds_alternative<EngineStats>(stats))
            Require(std::get<EngineStats>(stats).published_sequence == last_sequence + 1,
                    "a second recovery does not reapply records");
        const auto stored = engine->Get(written.front());
        Require(std::holds_alternative<StoredVector>(stored),
                "records stay readable across repeated opens");
        static_cast<void>(engine->Close());
    }
}

// The live count is a count of IDs, not of records. Two versions of one ID and
// one overwrite of a vector that already lives in a segment must not inflate it,
// and the value after a restart has to match the value before it.
void TestRecoveryLiveCountCountsIds() {
    auto fixture = Prepare("livecount");
    const VectorId fresh = 950001;
    const VectorId base = fixture.identifiers[0];
    std::uint64_t before = 0;
    {
        auto engine = OpenFixture(fixture, Options());
        if (!engine) return;
        Require(std::holds_alternative<WriteReceipt>(
                    engine->Insert(fresh, VectorFor(fixture, 0), WriteOptions{})),
                "live-count fixture insert");
        Require(std::holds_alternative<WriteReceipt>(
                    engine->Upsert(fresh, VectorFor(fixture, 1), WriteOptions{})),
                "live-count fixture second version");
        Require(std::holds_alternative<WriteReceipt>(
                    engine->Upsert(base, VectorFor(fixture, 2), WriteOptions{})),
                "live-count fixture overwrite of a base vector");
        const auto stats = engine->Stats();
        if (std::holds_alternative<EngineStats>(stats)) {
            Require(std::get<EngineStats>(stats).live_vectors == fixture.count + 1,
                    "a second version and a base overwrite do not add live vectors");
            before = std::get<EngineStats>(stats).live_vectors;
        }
        static_cast<void>(engine->Close());
    }
    {
        auto engine = OpenFixture(fixture, Options());
        if (!engine) return;
        const auto stats = engine->Stats();
        if (std::holds_alternative<EngineStats>(stats))
            Require(std::get<EngineStats>(stats).live_vectors == before,
                    "recovery counts IDs once, not once per WAL record");
        static_cast<void>(engine->Close());
    }
}

void TestTruncatedTailIsDiscarded() {
    auto fixture = Prepare("truncated");
    Sequence confirmed = 0;
    {
        auto engine = OpenFixture(fixture, Options());
        if (!engine) return;
        for (std::uint32_t index = 0; index < 4; ++index) {
            const auto receipt = engine->Insert(930000 + index, VectorFor(fixture, index),
                                                WriteOptions{});
            Require(std::holds_alternative<WriteReceipt>(receipt), "truncation fixture write");
            if (std::holds_alternative<WriteReceipt>(receipt))
                confirmed = std::get<WriteReceipt>(receipt).sequence;
        }
        static_cast<void>(engine->Close());
    }

    const auto files = WalFiles(fixture);
    Require(!files.empty(), "the truncation fixture has a WAL file");
    if (files.empty()) return;
    std::error_code error;
    const auto size = std::filesystem::file_size(files.back(), error);
    Require(!error && size > 20, "the WAL file has a writable tail");
    if (error || size <= 20) return;
    // Lop off half of the last frame: this is the shape of a process that died
    // mid-write. The record was never acknowledged, so it must not come back.
    std::filesystem::resize_file(files.back(), size - 12, error);
    Require(!error, "the last WAL frame can be truncated");

    {
        auto engine = OpenFixture(fixture, Options());
        if (!engine) return;
        const auto stats = engine->Stats();
        if (std::holds_alternative<EngineStats>(stats)) {
            Require(std::get<EngineStats>(stats).published_sequence == confirmed - 1,
                    "a torn tail drops exactly the unacknowledged record");
            Require(std::get<EngineStats>(stats).live_vectors == fixture.count + 3,
                    "the discarded record does not count as live");
        }
        Require(std::holds_alternative<Error>(engine->Get(930003)),
                "the discarded record is not visible");
        Require(std::holds_alternative<StoredVector>(engine->Get(930002)),
                "the acknowledged records before the tear survive");
        static_cast<void>(engine->Close());
    }
}

void TestRefusals() {
    auto fixture = Prepare("refusals");
    auto engine = OpenFixture(fixture, Options());
    if (!engine) return;

    std::vector<float> values = VectorFor(fixture, 1);
    Require(std::holds_alternative<Error>(engine->Insert(0, values, WriteOptions{})),
            "a zero ID is refused");
    std::vector<float> wrong(3, 0.0f);
    Require(std::holds_alternative<Error>(engine->Insert(940001, wrong, WriteOptions{})),
            "a mismatched dimension is refused");
    std::vector<float> not_finite(fixture.dimension, 1.0f);
    not_finite[2] = std::numeric_limits<float>::quiet_NaN();
    Require(std::holds_alternative<Error>(engine->Insert(940002, not_finite, WriteOptions{})),
            "a non-finite vector is refused");

    // Every refusal above must leave the index exactly as it was.
    const auto stats = engine->Stats();
    if (std::holds_alternative<EngineStats>(stats)) {
        Require(std::get<EngineStats>(stats).published_sequence == 0,
                "refused writes publish nothing");
        Require(std::get<EngineStats>(stats).live_vectors == fixture.count,
                "refused writes add no live vectors");
    }
    Require(std::holds_alternative<Error>(engine->Get(940001)),
            "a refused write is not readable");

    WriteOptions no_wait;
    no_wait.admission_timeout = std::chrono::milliseconds(0);
    Require(std::holds_alternative<Error>(engine->Insert(940003, values, no_wait)),
            "a zero admission timeout is refused");

    static_cast<void>(engine->Close());
    Require(std::holds_alternative<Error>(engine->Insert(940004, values, WriteOptions{})),
            "a closed engine refuses writes");
}

void TestDeltaCapacityIsEnforced() {
    auto fixture = Prepare("capacity");
    // Two records fit, the third cannot: the limit must be a refusal, never a
    // silent overwrite of an older record.
    const auto record_bytes = static_cast<std::uint64_t>(fixture.dimension) * sizeof(float) +
                              sizeof(VectorId) + sizeof(Sequence) + 32;
    auto engine = OpenFixture(fixture, Options(record_bytes * 2));
    if (!engine) return;
    Require(std::holds_alternative<WriteReceipt>(
                engine->Insert(950001, VectorFor(fixture, 0), WriteOptions{})),
            "the first write fits the delta");
    Require(std::holds_alternative<WriteReceipt>(
                engine->Insert(950002, VectorFor(fixture, 1), WriteOptions{})),
            "the second write fits the delta");
    const auto third = engine->Insert(950003, VectorFor(fixture, 2), WriteOptions{});
    Require(std::holds_alternative<Error>(third) &&
            std::get<Error>(third).code == ErrorCode::resource_exhausted,
            "a write beyond the delta capacity is refused");
    static_cast<void>(engine->Close());
}

// Silence is the failure mode that matters here: a damaged WAL that still opens
// would serve whatever survived and call it a complete history.
void TestCorruptFrameIsRefused() {
    auto fixture = Prepare("corrupt");
    {
        auto engine = OpenFixture(fixture, Options());
        if (!engine) return;
        for (std::uint32_t index = 0; index < 3; ++index)
            Require(std::holds_alternative<WriteReceipt>(
                        engine->Insert(970000 + index, VectorFor(fixture, index), WriteOptions{})),
                    "corruption fixture write");
        static_cast<void>(engine->Close());
    }
    const auto files = WalFiles(fixture);
    Require(!files.empty(), "the corruption fixture has a WAL file");
    if (files.empty()) return;

    // Flip one payload byte of the last frame. The CRC no longer matches, which
    // is indistinguishable from bit rot on the device.
    {
        std::fstream file(files.back(), std::ios::binary | std::ios::in | std::ios::out);
        Require(static_cast<bool>(file), "the WAL file reopens for editing");
        if (!file) return;
        file.seekg(-20, std::ios::end);
        char original = 0;
        file.read(&original, 1);
        file.seekp(-20, std::ios::end);
        const char flipped = static_cast<char>(original ^ 0x5A);
        file.write(&flipped, 1);
        file.flush();
    }

    const auto opened = Engine::Open(fixture.index, Options());
    Require(std::holds_alternative<Error>(opened) &&
            std::get<Error>(opened).code == ErrorCode::corruption,
            "a corrupted WAL frame fails the open instead of losing records silently");
}

// Rotation happens only between synchronised groups, so a short tail in an
// older file means acknowledged bytes were lost. That is corruption, not a
// crash tail.
void TestHistoricalShortTailIsRefused() {
    auto fixture = Prepare("history-tail");
    auto options = Options();
    // The writer refuses a rotation limit below one header plus one frame, so
    // both limits shrink together: two frames fill a file, the third rotates.
    options.wal.max_frame_bytes = 256;
    options.wal.rotation_bytes = 400;
    {
        auto engine = OpenFixture(fixture, options);
        if (!engine) return;
        for (std::uint32_t index = 0; index < 4; ++index)
            Require(std::holds_alternative<WriteReceipt>(
                        engine->Insert(980000 + index, VectorFor(fixture, index), WriteOptions{})),
                    "rotation fixture write");
        static_cast<void>(engine->Close());
    }
    const auto files = WalFiles(fixture);
    Require(files.size() >= 2, "a small rotation limit produces several WAL files");
    if (files.size() < 2) return;

    std::error_code error;
    const auto size = std::filesystem::file_size(files.front(), error);
    Require(!error && size > 10, "the historical WAL file has bytes to trim");
    if (error || size <= 10) return;
    std::filesystem::resize_file(files.front(), size - 10, error);
    Require(!error, "the historical WAL file can be truncated");

    const auto opened = Engine::Open(fixture.index, options);
    Require(std::holds_alternative<Error>(opened) &&
            std::get<Error>(opened).code == ErrorCode::corruption,
            "a short tail in a non-final WAL file is reported as corruption");
}

// Readers run while writes publish. Every result must be consistent with the
// snapshot it reported: no neighbour from a later sequence, no ID twice. This
// does not prove the absence of races, but it does exercise the handoff between
// the publication lock and the captured view on every iteration.
void TestConcurrentQueriesAndWrites() {
    auto fixture = Prepare("concurrent");
    auto engine = OpenFixture(fixture, Options());
    if (!engine) return;

    std::atomic<bool> stop{false};
    std::atomic<std::uint32_t> searches{0};
    std::atomic<std::uint32_t> failures{0};
    std::atomic<std::uint32_t> beyond_snapshot{0};
    std::atomic<std::uint32_t> duplicates{0};

    std::vector<std::thread> readers;
    readers.reserve(2);
    for (std::uint32_t worker = 0; worker < 2; ++worker) {
        readers.emplace_back([&, worker]() {
            std::uint64_t state = 11 + worker;
            while (!stop.load()) {
                std::vector<float> query(fixture.dimension);
                for (auto& value : query)
                    value = static_cast<float>(NextUnit(state) * 2.0 - 1.0);
                SearchOptions search;
                search.k = 8;
                search.candidate_limit = 64;
                const auto result = engine->Search(query, search);
                if (!std::holds_alternative<SearchResult>(result)) {
                    ++failures;
                    continue;
                }
                const auto& found = std::get<SearchResult>(result);
                ++searches;
                std::vector<VectorId> seen;
                for (const auto& neighbour : found.neighbors) {
                    if (neighbour.version > found.snapshot_sequence) ++beyond_snapshot;
                    if (std::find(seen.begin(), seen.end(), neighbour.id) != seen.end())
                        ++duplicates;
                    seen.push_back(neighbour.id);
                }
            }
        });
    }

    for (std::uint32_t index = 0; index < 40; ++index) {
        const auto receipt = engine->Insert(990000 + index, VectorFor(fixture, index),
                                            WriteOptions{});
        Require(std::holds_alternative<WriteReceipt>(receipt),
                "a write succeeds while readers are querying");
    }
    stop = true;
    for (auto& reader : readers) reader.join();

    Require(searches.load() > 0, "the concurrent readers completed searches");
    Require(failures.load() == 0, "no concurrent query failed");
    Require(beyond_snapshot.load() == 0,
            "no neighbour comes from beyond the snapshot the query reported");
    Require(duplicates.load() == 0, "no concurrent result repeats a logical ID");

    const auto stored = engine->Get(990000 + 39);
    Require(std::holds_alternative<StoredVector>(stored),
            "the last concurrent write is visible afterwards");
    static_cast<void>(engine->Close());
}

void TestUnsupportedCheckpointIsExplicit() {
    auto fixture = Prepare("checkpoint");
    auto engine = OpenFixture(fixture, Options());
    if (!engine) return;
    const auto receipt = engine->Checkpoint(std::chrono::milliseconds(1000));
    Require(std::holds_alternative<Error>(receipt) &&
            std::get<Error>(receipt).code == ErrorCode::unsupported,
            "checkpoint reports unsupported rather than pretending to flush");
    static_cast<void>(engine->Close());
}

// The child writes, acknowledges, and then dies without closing anything. Its
// exit is a process crash from the engine's point of view: no destructors, no
// final flush, handles released by the kernel.
int CrashChild(const char* index_path, int count) {
    auto opened = Engine::Open(std::filesystem::path(index_path), Options());
    if (!std::holds_alternative<std::unique_ptr<Engine>>(opened)) return 2;
    auto engine = std::get<std::unique_ptr<Engine>>(std::move(opened));
    std::vector<float> values(8, 0.5f);
    for (int index = 0; index < count; ++index) {
        const auto receipt = engine->Insert(960000 + static_cast<VectorId>(index), values,
                                            WriteOptions{});
        if (!std::holds_alternative<WriteReceipt>(receipt)) return 3;
        values[0] += 0.125f;
    }
    std::printf("ACKED %d\n", count);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 1);
    return 0;
}

void TestProcessCrashRecovery() {
    auto fixture = Prepare("crash");
    const int writes = 3;

    wchar_t executable[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, executable, MAX_PATH) == 0) {
        Require(false, "the test executable path is available");
        return;
    }
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --crash-child \"" +
        fixture.index.wstring() + L"\" " + std::to_wstring(writes);

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    Require(CreatePipe(&read_end, &write_end, &attributes, 0) != 0,
            "the crash test can create a pipe");
    if (!read_end) return;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0,
                                        nullptr, nullptr, &startup, &process);
    CloseHandle(write_end);
    if (!started) {
        CloseHandle(read_end);
        Require(false, "the crash child starts");
        return;
    }

    std::string output;
    char buffer[256];
    DWORD read_bytes = 0;
    // Read until the child says it has acknowledged the writes; only then does
    // killing it test recovery instead of testing a half-started process.
    while (output.find("ACKED") == std::string::npos) {
        if (ReadFile(read_end, buffer, sizeof(buffer), &read_bytes, nullptr) == 0) break;
        if (read_bytes == 0) break;
        output.append(buffer, read_bytes);
    }
    CloseHandle(read_end);
    if (output.find("ACKED") == std::string::npos)
        std::fprintf(stderr, "  crash child said: %s\n", output.c_str());
    Require(output.find("ACKED") != std::string::npos,
            "the crash child acknowledges its writes before dying");

    WaitForSingleObject(process.hProcess, 30000);
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Require(exit_code == 1, "the child was terminated instead of exiting cleanly");

    auto engine = OpenFixture(fixture, Options());
    if (!engine) return;
    const auto stats = engine->Stats();
    if (std::holds_alternative<EngineStats>(stats)) {
        Require(std::get<EngineStats>(stats).published_sequence == writes,
                "a crashed process leaves every acknowledged write recoverable");
        Require(std::get<EngineStats>(stats).live_vectors == fixture.count + writes,
                "the recovered live count matches the acknowledged writes");
    }
    for (int index = 0; index < writes; ++index) {
        const auto stored = engine->Get(960000 + static_cast<VectorId>(index));
        Require(std::holds_alternative<StoredVector>(stored),
                "each acknowledged write is readable after the crash");
    }
    // Recovery has to leave the engine writable, not merely readable.
    const auto next = engine->Insert(969999, std::vector<float>(8, 0.75f), WriteOptions{});
    Require(std::holds_alternative<WriteReceipt>(next),
            "the engine accepts writes after crash recovery");
    if (std::holds_alternative<WriteReceipt>(next))
        Require(std::get<WriteReceipt>(next).sequence == writes + 1,
                "the sequence continues after a crash");
    static_cast<void>(engine->Close());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "--crash-child")
        return CrashChild(argv[2], std::atoi(argv[3]));

    TestConfirmThenVisible();
    TestVersionsAndConflict();
    TestSnapshotIsolation();
    TestConcurrentQueriesAndWrites();
    TestRecoveryReplaysConfirmedWrites();
    TestRecoveryLiveCountCountsIds();
    TestTruncatedTailIsDiscarded();
    TestCorruptFrameIsRefused();
    TestHistoricalShortTailIsRefused();
    TestRefusals();
    TestDeltaCapacityIsEnforced();
    TestUnsupportedCheckpointIsExplicit();
    TestProcessCrashRecovery();

    std::printf("dynamic-engine: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d dynamic engine checks failed\n", g_failures);
        return 1;
    }
    return 0;
}
