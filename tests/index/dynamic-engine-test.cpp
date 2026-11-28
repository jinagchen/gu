// Stage 6 B checks: durable writes, snapshot visibility, recovery and the
// refusal paths that must not leave a half-applied record behind.
//
// Confirmed writes must survive restart, and complete frames can also be
// recovered when the caller never received a receipt.

#include "agent_io/engine.hpp"
#include "agent_io/index.hpp"
#include "agent_io/storage.hpp"

#include "../../src/index/builder.hpp"
#include "../../src/index/format.hpp"
#include "../../src/index/recovery.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <semaphore>
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

template<class T> const Error* AsError(const Result<T>& result) {
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

constexpr VectorId kTailConfirmed = 930001;
constexpr VectorId kTailTorn = 930002;
constexpr VectorId kTailContinued = 930003;
constexpr DWORD kTailChildExit = 43;
const std::vector<float> kFirstValues(8, 0.25f);
const std::vector<float> kNextValues(8, 0.75f);

bool HasVector(Engine& engine, VectorId id, Sequence version, const std::vector<float>& values) {
    const auto result = engine.Get(id);
    const auto* vector = std::get_if<StoredVector>(&result);
    return vector && vector->id == id && vector->version == version && vector->values == values;
}

bool HasReceipt(const Result<WriteReceipt>& result, VectorId id, Sequence sequence) {
    const auto* receipt = std::get_if<WriteReceipt>(&result);
    return receipt && receipt->id == id && receipt->sequence == sequence;
}

bool AppendFramePrefix(const std::filesystem::path& path, Sequence sequence, VectorId id,
                       const std::vector<float>& values, std::size_t missing) {
    std::vector<std::byte> frame(static_cast<std::size_t>(
        std::get<std::uint64_t>(WalFrameBytes(static_cast<std::uint32_t>(values.size())))));
    if (AsError(EncodeWalFrame({WalOperation::insert, sequence, id, values}, frame)) ||
        frame.size() <= missing) return false;
    std::ofstream output(path, std::ios::binary | std::ios::app);
    output.write(reinterpret_cast<const char*>(frame.data()),
                 static_cast<std::streamsize>(frame.size() - missing));
    output.close();
    return static_cast<bool>(output);
}

std::string ReadBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    Require(static_cast<bool>(input), "read test fixture file");
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

auto WalContents(const Fixture& fixture) {
    std::vector<std::pair<std::filesystem::path, std::string>> contents;
    for (const auto& path : WalFiles(fixture)) contents.emplace_back(path, ReadBytes(path));
    return contents;
}

InstanceId FixtureInstance(const Fixture& fixture) {
    return std::get<CurrentRecord>(DecodeCurrent(ReadBytes(fixture.index / "CURRENT"))).instance_id;
}

std::filesystem::path WriteWalHeader(const Fixture& fixture, const char* name,
                                     InstanceId instance, Sequence start) {
    const auto path = fixture.index / "wal" / name;
    std::filesystem::create_directories(path.parent_path());
    std::array<std::byte, wal_detail::kWalHeaderBytes> header{};
    Require(!AsError(EncodeWalFileHeader({instance, start}, header)), "encode fixture WAL header");
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));
    output.close();
    Require(static_cast<bool>(output), "write fixture WAL header");
    return path;
}

void ExpectUnchangedCorruption(const Fixture& fixture, const EngineOptions& options) {
    const auto before = WalContents(fixture);
    const auto opened = Engine::Open(fixture.index, options);
    Require(AsError(opened) && AsError(opened)->code == ErrorCode::corruption,
            "invalid WAL history must fail Open with corruption");
    Require(WalContents(fixture) == before, "rejected WAL files remain byte-for-byte identical");
}

enum class NativeFault { none, seek, truncate, flush, publish, attach, kill_on_recovery_flush,
                         kill_after_frame_flush, short_write, append_error, partial_disk_full };
NativeFault g_wal_fault = NativeFault::none;
unsigned g_wal_seeks = 0, g_wal_truncates = 0, g_wal_flushes = 0;
unsigned g_wal_writes = 0;
constexpr DWORD kRecoveryCrashExit = 44;
constexpr DWORD kUnrepliedCrashExit = 45;
thread_local std::binary_semaphore* g_flush_entered = nullptr;
thread_local std::binary_semaphore* g_flush_resume = nullptr;

BOOL WINAPI TestWalWrite(HANDLE file, LPCVOID data, DWORD bytes, LPDWORD done, LPOVERLAPPED io) {
    const auto call = ++g_wal_writes;
    if (g_wal_fault == NativeFault::append_error ||
        (g_wal_fault == NativeFault::partial_disk_full && call > 1)) {
        SetLastError(g_wal_fault == NativeFault::append_error ? ERROR_WRITE_FAULT : ERROR_DISK_FULL);
        return FALSE;
    }
    if ((g_wal_fault == NativeFault::short_write ||
         g_wal_fault == NativeFault::partial_disk_full) && call == 1)
        bytes = (std::min)(bytes, DWORD{12});
    return WriteFile(file, data, bytes, done, io);
}

BOOL WINAPI TestWalSeek(HANDLE file, LARGE_INTEGER offset, PLARGE_INTEGER result, DWORD mode) {
    ++g_wal_seeks;
    if (g_wal_fault != NativeFault::seek) return SetFilePointerEx(file, offset, result, mode);
    SetLastError(ERROR_SEEK);
    return FALSE;
}

BOOL WINAPI TestWalTruncate(HANDLE file) {
    ++g_wal_truncates;
    if (g_wal_fault != NativeFault::truncate) return SetEndOfFile(file);
    SetLastError(ERROR_DISK_FULL);
    return FALSE;
}

BOOL WINAPI TestWalFlush(HANDLE file) {
    ++g_wal_flushes;
    if (g_flush_entered) {
        g_flush_entered->release();
        g_flush_entered = nullptr;
        g_flush_resume->acquire();
    }
    if (g_wal_fault == NativeFault::flush) {
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    if (g_wal_fault == NativeFault::kill_on_recovery_flush && g_wal_truncates == 1)
        TerminateProcess(GetCurrentProcess(), kRecoveryCrashExit);
    const auto result = FlushFileBuffers(file);
    if (result && g_wal_fault == NativeFault::kill_after_frame_flush && g_wal_flushes == 2)
        TerminateProcess(GetCurrentProcess(), kUnrepliedCrashExit);
    return result;
}

BOOL WINAPI TestWalPublish(LPCWSTR pending, LPCWSTR target, DWORD flags) {
    if (g_wal_fault != NativeFault::publish) return MoveFileExW(pending, target, flags);
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

HANDLE WINAPI TestWalOpen(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attr,
                          DWORD disposition, DWORD flags, HANDLE templ) {
    if (g_wal_fault != NativeFault::attach || disposition != OPEN_EXISTING)
        return CreateFileW(path, access, share, attr, disposition, flags, templ);
    SetLastError(ERROR_SHARING_VIOLATION);
    return INVALID_HANDLE_VALUE;
}

struct WalFaultScope {
    wal_detail::NativeIo prior = wal_detail::CurrentNativeIo();
    explicit WalFaultScope(NativeFault fault) {
        g_wal_fault = fault;
        g_wal_seeks = g_wal_truncates = g_wal_flushes = g_wal_writes = 0;
        auto& io = wal_detail::CurrentNativeIo();
        io.write = TestWalWrite;
        io.seek = TestWalSeek;
        io.truncate = TestWalTruncate;
        io.flush = TestWalFlush;
        io.publish = TestWalPublish;
        io.open = TestWalOpen;
    }
    ~WalFaultScope() {
        wal_detail::CurrentNativeIo() = prior;
        g_wal_fault = NativeFault::none;
    }
};

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

int TailRecoveryChild(const char* index_path) {
    auto opened = Engine::Open(index_path, Options());
    auto* engine = std::get_if<std::unique_ptr<Engine>>(&opened);
    if (!engine) return 2;
    const auto stats = (*engine)->Stats();
    if (!HasVector(**engine, kTailConfirmed, 1, kFirstValues) ||
        std::get<EngineStats>(stats).published_sequence != 1 ||
        !std::holds_alternative<Error>((*engine)->Get(kTailTorn))) return 3;
    if (!HasReceipt((*engine)->Insert(kTailContinued, kNextValues, {}), kTailContinued, 2))
        return 4;
    TerminateProcess(GetCurrentProcess(), kTailChildExit);
    return 5;
}

DWORD RunTerminatingChild(const Fixture& fixture, const wchar_t* mode) {
    wchar_t executable[MAX_PATH]{};
    const auto length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return 0;
    auto command = L"\"" + std::wstring(executable) + L"\" " + mode + L" \"" +
        fixture.index.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &child)) return 0;
    CloseHandle(child.hThread);
    DWORD code = 0;
    if (WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0) {
        GetExitCodeProcess(child.hProcess, &code);
    } else {
        TerminateProcess(child.hProcess, 1);
        WaitForSingleObject(child.hProcess, 5000);
    }
    CloseHandle(child.hProcess);
    return code;
}

void TestTornTailThenConfirmedWriteSurvivesAnotherOpen() {
    auto fixture = Prepare("tail-continue");
    {
        auto engine = OpenFixture(fixture, Options());
        if (!engine) return;
        Require(HasReceipt(engine->Insert(kTailConfirmed, kFirstValues, {}), kTailConfirmed, 1),
                "the first record is acknowledged before simulating a torn append");
    }
    const auto files = WalFiles(fixture);
    Require(files.size() == 1, "one WAL holds the acknowledged record");
    if (files.size() != 1) return;
    Require(AppendFramePrefix(files.front(), 2, kTailTorn, kNextValues, 12),
            "append only a legal prefix of the unacknowledged frame");
    const auto exit_code = RunTerminatingChild(fixture, L"--tail-recovery-child");
    Require(exit_code == kTailChildExit,
            "first recovery restores sequence 1, then acknowledges sequence 2 before forced exit");
    if (exit_code != kTailChildExit) return;

    auto engine = OpenFixture(fixture, Options());
    if (!engine) return;
    Require(HasVector(*engine, kTailConfirmed, 1, kFirstValues), "first receipt survives");
    Require(HasVector(*engine, kTailContinued, 2, kNextValues), "second receipt survives");
    Require(std::holds_alternative<Error>(engine->Get(kTailTorn)), "torn ID is still absent");
    Require(HasReceipt(engine->Insert(930004, kFirstValues, {}), 930004, 3),
            "second recovery continues at sequence 3");
}

void TestEmptyLatestWalIsAdopted() {
    for (const bool torn : {false, true}) {
        auto fixture = Prepare(torn ? "first-frame-torn" : "empty-wal");
        const auto path = WriteWalHeader(fixture, "empty.log", FixtureInstance(fixture), 1);
        const auto header = ReadBytes(path);
        if (torn) Require(AppendFramePrefix(path, 1, kTailTorn, kFirstValues, 12),
                          "append a prefix of the first frame");
        for (int pass = 0; pass < 2; ++pass) {
            auto engine = OpenFixture(fixture, Options());
            if (!engine) return;
            Require(std::get<EngineStats>(engine->Stats()).published_sequence == 0,
                    "an empty WAL does not advance the snapshot");
        }
        Require(WalFiles(fixture) == std::vector{path} && ReadBytes(path) == header,
                "repeated opens preserve the one empty header");
        {
            auto engine = OpenFixture(fixture, Options());
            if (!engine) return;
            Require(HasReceipt(engine->Insert(kTailConfirmed, kFirstValues, {}), kTailConfirmed, 1),
                    "the adopted WAL receives its first frame");
        }
        Require(WalFiles(fixture) == std::vector{path}, "the original file was adopted");
        auto engine = OpenFixture(fixture, Options());
        if (!engine) return;
        Require(HasVector(*engine, kTailConfirmed, 1, kFirstValues), "adopted WAL replays");
        Require(std::holds_alternative<Error>(engine->Get(kTailTorn)), "torn first ID never appears");
    }
}

void TestRotatedEmptyWalIsAdopted() {
    auto fixture = Prepare("rotated-torn");
    auto options = Options();
    options.wal.max_frame_bytes = 104;
    options.wal.rotation_bytes = 64 + 104;
    {
        WalFaultScope fault(NativeFault::none);
        auto engine = OpenFixture(fixture, options);
        if (!engine) return;
        Require(HasReceipt(engine->Insert(kTailConfirmed, kFirstValues, {}), kTailConfirmed, 1),
                "first frame fills the WAL up to the rotation limit");
        g_wal_fault = NativeFault::attach;
        const auto failure = engine->Insert(kTailTorn, kFirstValues, {});
        Require(AsError(failure) && AsError(failure)->code == ErrorCode::io_error &&
                AsError(failure)->system_error == ERROR_SHARING_VIOLATION &&
                AsError(failure)->write_effect == WriteEffect::none,
                "rotation fails before a frame if the published log cannot be attached");
        g_wal_fault = NativeFault::none;
        const auto refused = engine->Insert(kTailContinued, kNextValues, {});
        Require(AsError(refused) && AsError(refused)->code == ErrorCode::recovery_required,
                "rotation attachment failure blocks writes until recovery");
    }
    const auto files = WalFiles(fixture);
    Require(files.size() == 2, "one empty log was published by rotation");
    if (files.size() != 2) return;
    const auto original = ReadBytes(files.front());
    const auto header = ReadBytes(files.back());
    Require(header.size() == wal_detail::kWalHeaderBytes, "rotated log contains only its header");
    Require(AppendFramePrefix(files.back(), 2, kTailTorn, kFirstValues, 72),
            "append a 32-byte first frame prefix to the rotated WAL");
    {
        auto engine = OpenFixture(fixture, options);
        if (!engine) return;
    }
    Require(WalFiles(fixture) == files && ReadBytes(files.back()) == header,
            "first frame in the rotated file is repaired back to the same header");
    {
        auto engine = OpenFixture(fixture, options);
        if (!engine) return;
        Require(HasReceipt(engine->Insert(kTailContinued, kNextValues, {}), kTailContinued, 2),
                "continued sequence 2 is written into the rotated WAL");
    }
    Require(WalFiles(fixture) == files && ReadBytes(files.front()) == original,
            "continuation retains both original files and leaves the older one unchanged");
    auto engine = OpenFixture(fixture, options);
    if (!engine) return;
    Require(HasVector(*engine, kTailConfirmed, 1, kFirstValues) &&
            HasVector(*engine, kTailContinued, 2, kNextValues),
            "both confirmed frames replay across rotation");
}

void TestWriteFaultsAtEngineBoundary() {
    for (const auto native : {NativeFault::short_write, NativeFault::append_error,
                              NativeFault::partial_disk_full, NativeFault::flush}) {
        auto fixture = Prepare("write-fault");
        WalFaultScope fault(NativeFault::none);
        auto engine = OpenFixture(fixture, Options());
        if (!engine) return;
        Require(HasReceipt(engine->Insert(kTailConfirmed, kFirstValues, {}), kTailConfirmed, 1),
                "an acknowledged prefix exists before the write fault");
        g_wal_fault = native;
        g_wal_writes = 0;
        const auto attempted = engine->Insert(kTailTorn, kNextValues, {});
        if (native == NativeFault::short_write) {
            Require(g_wal_writes == 2 && HasReceipt(attempted, kTailTorn, 2),
                    "a successful short write is completed before acknowledgment");
        } else {
            const auto* error = AsError(attempted);
            const auto expected = native == NativeFault::partial_disk_full
                ? ERROR_DISK_FULL : ERROR_WRITE_FAULT;
            Require(error && error->code == ErrorCode::io_error &&
                        error->system_error == expected && error->attempted_sequence == 2 &&
                        error->write_effect == WriteEffect::may_have_applied,
                    "failed append or sync reports the attempted sequence as uncertain");
            Require(AsError(engine->Get(kTailTorn)) != nullptr,
                    "an uncertain write is not visible in the running engine");
            const auto refused = engine->Insert(kTailContinued, kFirstValues, {});
            const auto* refused_error = AsError(refused);
            Require(refused_error && refused_error->code == ErrorCode::recovery_required &&
                        refused_error->write_effect == WriteEffect::none &&
                        !refused_error->attempted_sequence,
                    "a later request stops before getting a sequence or writing bytes");
        }
        g_wal_fault = NativeFault::none;
        engine.reset();
        engine = OpenFixture(fixture, Options());
        if (!engine) return;
        Require(HasVector(*engine, kTailConfirmed, 1, kFirstValues),
                "the acknowledged prefix survives the write fault");
        const bool complete = native == NativeFault::short_write || native == NativeFault::flush;
        if (complete)
            Require(HasVector(*engine, kTailTorn, 2, kNextValues),
                    "recovery replays a complete frame, including a failed sync");
        else
            Require(AsError(engine->Get(kTailTorn)) != nullptr,
                    "recovery discards a missing or partial frame");
        Require(HasReceipt(engine->Insert(kTailContinued, kFirstValues, {}),
                           kTailContinued, complete ? 3 : 2),
                "recovery continues at the sequence after the complete prefix");
    }
}

void TestWriteAdmissionTimeoutAndClose() {
    using namespace std::chrono_literals;
    auto fixture = Prepare("admission-close");
    auto options = Options();
    options.max_pending_writes = 1;
    auto engine = OpenFixture(fixture, options);
    if (!engine) return;
    std::binary_semaphore entered(0), resume(0);
    auto active = std::async(std::launch::async, [&] {
        WalFaultScope fault(NativeFault::none);
        g_flush_entered = &entered;
        g_flush_resume = &resume;
        return engine->Insert(kTailConfirmed, kFirstValues, {});
    });
    const bool stalled = entered.try_acquire_for(5s);
    Require(stalled, "first write occupies the only admission slot");
    if (!stalled) {
        resume.release();
        return;
    }
    const auto timed_out = engine->Insert(kTailTorn, kNextValues, WriteOptions{20ms});
    const auto* error = AsError(timed_out);
    Require(error && error->code == ErrorCode::resource_exhausted &&
                !error->attempted_sequence,
            "capacity wait times out before writing any WAL bytes");

    auto queued = std::async(std::launch::async, [&] {
        return engine->Insert(kTailContinued, kNextValues, WriteOptions{30s});
    });
    auto closing = std::async(std::launch::async, [&] { return engine->Close(); });
    const bool woke = queued.wait_for(5s) == std::future_status::ready;
    resume.release();
    Require(woke, "closing wakes a waiting writer while the active one is stalled");
    const auto closed = queued.get();
    Require(AsError(closed) && AsError(closed)->code == ErrorCode::closed,
            "the queued writer is refused during close");
    Require(HasReceipt(active.get(), kTailConfirmed, 1),
            "an already active write can finish before close");
    Require(!AsError(closing.get()), "close completes");
}

void TestViewAdmissionHonorsDeadlines() {
    using namespace std::chrono_literals;
    auto fixture = Prepare("view-admission");
    auto options = Options();
    options.max_pending_writes = 2;
    auto engine = OpenFixture(fixture, options);
    if (!engine) return;

    std::binary_semaphore entered(0), resume(0);
    auto active = std::async(std::launch::async, [&] {
        WalFaultScope fault(NativeFault::none);
        g_flush_entered = &entered;
        g_flush_resume = &resume;
        return engine->Insert(kTailConfirmed, kFirstValues, {});
    });
    const bool stalled = entered.try_acquire_for(5s);
    Require(stalled, "a write holds the view during WAL sync");
    if (!stalled) {
        resume.release();
        return;
    }

    auto search = std::async(std::launch::async, [&] {
        SearchOptions request;
        request.timeout = 25ms;
        return engine->Search(kFirstValues, request);
    });
    auto write = std::async(std::launch::async, [&] {
        return engine->Insert(kTailTorn, kNextValues, WriteOptions{25ms});
    });
    const bool search_completed = search.wait_for(3s) == std::future_status::ready;
    const bool write_completed = write.wait_for(3s) == std::future_status::ready;
    resume.release();

    const auto search_result = search.get();
    const auto write_result = write.get();
    Require(search_completed && AsError(search_result) &&
                AsError(search_result)->code == ErrorCode::deadline_exceeded,
            "a query times out while the view is still locked");
    const auto* error = AsError(write_result);
    Require(write_completed && error && error->code == ErrorCode::deadline_exceeded &&
                error->write_effect == WriteEffect::none && !error->attempted_sequence,
            "a write times out after admission without attempting a WAL frame");
    Require(HasReceipt(active.get(), kTailConfirmed, 1) &&
                HasReceipt(engine->Insert(kTailTorn, kNextValues, {}), kTailTorn, 2),
            "the next attempt uses the next contiguous WAL sequence");
}

void TestRecoveryIoFailuresAndRetry() {
    auto fixture = Prepare("tail-retry");
    {
        auto engine = OpenFixture(fixture, Options());
        if (!engine) return;
        Require(HasReceipt(engine->Insert(kTailConfirmed, kFirstValues, {}), kTailConfirmed, 1),
                "repair fixture has a confirmed prefix");
    }
    const auto clean = WalContents(fixture);
    if (clean.size() != 1) return;
    const auto& path = clean.front().first;
    Require(AppendFramePrefix(path, 2, kTailTorn, kNextValues, 12), "simulate a torn append");
    const auto torn = WalContents(fixture);
    const auto too_small = Engine::Open(fixture.index, Options(0));
    Require(AsError(too_small) && AsError(too_small)->code == ErrorCode::resource_exhausted &&
            WalContents(fixture) == torn, "budget failure occurs before tail repair");

    const auto fails = [&](NativeFault fault, DWORD native, unsigned seeks,
                           unsigned truncates, unsigned flushes) {
        WalFaultScope injection(fault);
        const auto opened = Engine::Open(fixture.index, Options());
        const auto* error = AsError(opened);
        Require(error && error->code == ErrorCode::io_error &&
                error->system_error == static_cast<int>(native),
                "Open reports the injected native error and returns no engine");
        Require(g_wal_seeks == seeks && g_wal_truncates == truncates && g_wal_flushes == flushes,
                "repair fails at the intended I/O step");
    };
    fails(NativeFault::seek, ERROR_SEEK, 1, 0, 0);
    Require(WalContents(fixture) == torn, "seek failure leaves all bytes untouched");
    fails(NativeFault::truncate, ERROR_DISK_FULL, 1, 1, 0);
    Require(WalContents(fixture) == torn, "truncate failure leaves all bytes untouched");
    fails(NativeFault::flush, ERROR_WRITE_FAULT, 1, 1, 1);
    Require(WalContents(fixture) == clean, "truncation succeeded even though its flush failed");
    fails(NativeFault::flush, ERROR_WRITE_FAULT, 0, 0, 1);
    Require(WalContents(fixture) == clean, "recovery retries the flush at the clean length");

    Require(AppendFramePrefix(path, 2, kTailTorn, kNextValues, 12), "restore the torn append");
    Require(RunTerminatingChild(fixture, L"--truncate-child") == kRecoveryCrashExit,
            "child is killed after truncating and before flushing");
    Require(WalContents(fixture) == clean, "process exit leaves a WAL at the repaired length");
    fails(NativeFault::flush, ERROR_WRITE_FAULT, 0, 0, 1);
    WalFaultScope injection(NativeFault::none);
    auto engine = OpenFixture(fixture, Options());
    if (!engine) return;
    Require(g_wal_flushes == 1 && g_wal_truncates == 0, "successful Open syncs the clean length");
    Require(HasVector(*engine, kTailConfirmed, 1, kFirstValues) &&
            HasReceipt(engine->Insert(kTailContinued, kNextValues, {}), kTailContinued, 2),
            "successful retry restores the confirmed prefix and permits the next write");
}

int RecoveryFaultChild(const char* index_path, NativeFault fault) {
    WalFaultScope injection(fault);
    auto opened = Engine::Open(index_path, Options());
    if (auto* engine = std::get_if<std::unique_ptr<Engine>>(&opened);
        engine && fault == NativeFault::kill_after_frame_flush)
        static_cast<void>((*engine)->Insert(kTailConfirmed, kFirstValues, {}));
    return 2;
}

void TestCompleteUnrepliedFrameReplays() {
    auto fixture = Prepare("unreplied");
    Require(RunTerminatingChild(fixture, L"--unreplied-child") == kUnrepliedCrashExit,
            "child is killed inside a successful frame flush before returning the receipt");
    auto engine = OpenFixture(fixture, Options());
    if (!engine) return;
    Require(HasVector(*engine, kTailConfirmed, 1, kFirstValues),
            "a complete synced frame with no returned receipt is replayed");
    Require(HasReceipt(engine->Insert(kTailContinued, kNextValues, {}), kTailContinued, 2),
            "writer continues after replaying the unreplied frame");
}

void TestNewWalPublicationFailures() {
    for (const NativeFault mode : {NativeFault::publish, NativeFault::attach}) {
        const bool published = mode == NativeFault::attach;
        auto fixture = Prepare(published ? "attach-failure" : "rename-failure");
        {
            WalFaultScope injection(mode);
            auto engine = OpenFixture(fixture, Options());
            if (!engine) return;
            const auto rejected = engine->Insert(kTailConfirmed, kFirstValues, {});
            Require(AsError(rejected) && AsError(rejected)->code == ErrorCode::io_error &&
                    AsError(rejected)->write_effect == WriteEffect::none &&
                    AsError(rejected)->system_error ==
                        (published ? ERROR_SHARING_VIOLATION : ERROR_ACCESS_DENIED),
                    "publication or attachment failure returns its native error without applying");
            Require(std::get<EngineStats>(engine->Stats()).published_sequence == 0 &&
                    std::holds_alternative<Error>(engine->Get(kTailConfirmed)),
                    "failed publication never publishes the record in memory");
            g_wal_fault = NativeFault::none;
            const auto next = engine->Insert(kTailContinued, kNextValues, {});
            Require(AsError(next) && AsError(next)->code == ErrorCode::recovery_required,
                    "same engine cannot create a second WAL after uncertain creation");
        }
        const auto files = WalFiles(fixture);
        Require(files.size() == (published ? 1u : 0u), "only successful renames expose a .log");
        if (published && files.size() == 1)
            Require(std::filesystem::file_size(files.front()) == wal_detail::kWalHeaderBytes,
                    "no business frame was appended before attaching the log");
        const auto partial = fixture.index / "wal" / "short.pending";
        std::ofstream(partial, std::ios::binary).put('A');
        {
            auto engine = OpenFixture(fixture, Options());
            if (!engine) return;
            Require(HasReceipt(engine->Insert(kTailContinued, kNextValues, {}), kTailContinued, 1),
                    "reopen ignores pending files and resumes at sequence 1");
        }
        const auto final = WalFiles(fixture);
        Require(final.size() == 1 && (!published || final == files),
                "reopen adopts any already-published empty log without duplication");
        Require(ReadBytes(partial) == "A", "recovery leaves the pending file untouched");
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
    const auto before = WalContents(fixture);
    const auto constrained = Engine::Open(fixture.index, Options(record_bytes));
    const auto* error = AsError(constrained);
    Require(error && error->code == ErrorCode::resource_exhausted,
            "recovery refuses a replay larger than the delta limit");
    Require(WalContents(fixture) == before, "recovery budget refusal preserves WAL bytes");
    engine = OpenFixture(fixture, Options(record_bytes * 2));
    if (!engine) return;
    Require(HasVector(*engine, 950001, 1, VectorFor(fixture, 0)) &&
                HasVector(*engine, 950002, 2, VectorFor(fixture, 1)),
            "both writes recover when the replay budget is restored");
}

void TestWalHistoryWorkspaceLimit() {
    auto fixture = Prepare("recovery-workspace");
    const auto instance = FixtureInstance(fixture);
    const auto values = VectorFor(fixture, 0);
    constexpr Sequence records = 24;
    for (Sequence sequence = 1; sequence <= records; ++sequence) {
        const auto name = "history-" + std::to_string(sequence) + ".log";
        const auto path = WriteWalHeader(fixture, name.c_str(), instance, sequence);
        Require(AppendFramePrefix(path, sequence, 960000 + sequence, values, 0),
                "create complete historical WAL frame");
    }
    const auto before = WalContents(fixture);
    auto options = Options();
    WalRecoveryLimits limits;
    limits.max_workspace_bytes = 4096;
    const auto constrained = ScanWalDirectory(fixture.index / "wal", instance, fixture.dimension,
                                              0, options.wal, limits);
    const auto* error = AsError(constrained);
    Require(error && error->code == ErrorCode::resource_exhausted &&
                error->message.find("workspace") != std::string::npos,
            "WAL history cannot exceed its configured workspace budget");
    Require(WalContents(fixture) == before, "budget refusal leaves historical WAL bytes intact");
    auto engine = OpenFixture(fixture, options);
    if (!engine) return;
    Require(HasVector(*engine, 960001, 1, values) &&
                HasVector(*engine, 960000 + records, records, values),
            "the confirmed prefix and tail recover under the ordinary workspace budget");
}

constexpr Sequence kRecoveryStudyFiles = 1024;
constexpr Sequence kRecoveryStudyFramesPerFile = 8;
constexpr VectorId kRecoveryStudyFirstId = 980000;

int PrepareRecoveryStudy() {
    const auto fixture = Prepare("recovery-study");
    if (g_failures != 0) return 2;
    const auto instance = FixtureInstance(fixture);
    for (Sequence file = 0; file < kRecoveryStudyFiles; ++file) {
        const auto first = file * kRecoveryStudyFramesPerFile + 1;
        const auto name = "history-" + std::to_string(first) + ".log";
        const auto path = WriteWalHeader(fixture, name.c_str(), instance, first);
        for (Sequence sequence = first; sequence < first + kRecoveryStudyFramesPerFile; ++sequence)
            if (!AppendFramePrefix(path, sequence, kRecoveryStudyFirstId + sequence,
                                   kFirstValues, 0))
                return 3;
    }
    std::puts(fixture.index.string().c_str());
    return g_failures ? 4 : 0;
}

int MeasureRecoveryStudy(const char* index_path) {
    constexpr auto frames = kRecoveryStudyFiles * kRecoveryStudyFramesPerFile;
    const auto begin = std::chrono::steady_clock::now();
    auto opened = Engine::Open(index_path, Options());
    const auto elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - begin).count();
    const auto* engine = std::get_if<std::unique_ptr<Engine>>(&opened);
    if (!engine) {
        std::fprintf(stderr, "%s\n", std::get<Error>(opened).message.c_str());
        return 2;
    }
    const auto stats_result = (*engine)->Stats();
    const auto* stats = std::get_if<EngineStats>(&stats_result);
    if (!stats || stats->published_sequence != frames ||
        !HasVector(**engine, kRecoveryStudyFirstId + 1, 1, kFirstValues) ||
        !HasVector(**engine, kRecoveryStudyFirstId + frames, frames, kFirstValues))
        return 3;
    std::printf("files=%llu frames=%llu open_ms=%.3f published_sequence=%llu\n",
                static_cast<unsigned long long>(kRecoveryStudyFiles),
                static_cast<unsigned long long>(frames), elapsed_ms,
                static_cast<unsigned long long>(stats->published_sequence));
    return 0;
}

void TestRestartReclaimsOnlyCoveredWal() {
    using namespace std::chrono_literals;
    auto fixture = Prepare("reclaim-covered-wal");
    constexpr VectorId covered_id = 966001;
    constexpr VectorId tail_id = 966002;
    const auto values = VectorFor(fixture, 0);
    auto engine = OpenFixture(fixture, Options());
    if (!engine) return;
    Require(HasReceipt(engine->Insert(covered_id, values, {}), covered_id, 1),
            "covered write is confirmed");
    Require(std::holds_alternative<CheckpointReceipt>(engine->Checkpoint(5s)),
            "covered write reaches the index");
    Require(HasReceipt(engine->Insert(tail_id, values, {}), tail_id, 2),
            "uncovered write is confirmed in its own WAL");
    static_cast<void>(engine->Close());

    const auto stale = WriteWalHeader(fixture, "covered.log", FixtureInstance(fixture), 1);
    Require(AppendFramePrefix(stale, 1, covered_id, values, 0),
            "restore a complete covered file as if its deletion failed");
    engine = OpenFixture(fixture, Options());
    if (!engine) return;
    Require(!std::filesystem::exists(stale) && WalFiles(fixture).size() == 1,
            "recovery retires the covered log and leaves the uncovered one");
    Require(HasVector(*engine, covered_id, 1, values) && HasVector(*engine, tail_id, 2, values),
            "the stored and replayed writes stay visible");
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

    ExpectUnchangedCorruption(fixture, Options());
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

    ExpectUnchangedCorruption(fixture, options);
}

void TestInvalidHistoryRemainsUnchanged() {
    for (const std::string_view scenario : {"foreign", "gap", "duplicate", "bad-tail-sequence",
                                            "bad-tail-crc", "leading-gap", "short-header"}) {
        auto fixture = Prepare(std::string(scenario));
        const auto instance = FixtureInstance(fixture);
        if (scenario == "leading-gap") {
            const auto path = WriteWalHeader(fixture, "leading.log", instance, 2);
            Require(AppendFramePrefix(path, 2, kTailTorn, kNextValues, 0),
                    "leading sequence gap has a fully valid frame");
        } else {
            {
                auto engine = OpenFixture(fixture, Options());
                if (!engine) return;
                Require(HasReceipt(engine->Insert(kTailConfirmed, kFirstValues, {}),
                                   kTailConfirmed, 1), "invalid-history fixture has a good prefix");
            }
            const auto files = WalFiles(fixture);
            if (files.size() != 1) return;
            if (scenario == "bad-tail-sequence" || scenario == "bad-tail-crc") {
                const bool wrong_sequence = scenario == "bad-tail-sequence";
                Require(AppendFramePrefix(files.front(), wrong_sequence ? 3 : 2, kTailTorn,
                                          kNextValues, wrong_sequence ? 79 : 7),
                        "append a prefix containing one sequence or CRC byte");
                if (!wrong_sequence) {
                    const auto bytes = ReadBytes(files.front());
                    std::fstream file(files.front(), std::ios::in | std::ios::out | std::ios::binary);
                    file.seekp(-1, std::ios::end);
                    file.put(static_cast<char>(bytes.back() ^ 1));
                    file.close();
                    Require(static_cast<bool>(file), "corrupt the first present CRC byte");
                }
            } else {
                auto identity = instance;
                if (scenario == "foreign") identity.front() ^= std::byte{1};
                const Sequence start = scenario == "duplicate" ? 1 : (scenario == "gap" ? 3 : 2);
                const auto path = WriteWalHeader(fixture, "invalid.log", identity, start);
                if (scenario == "short-header") {
                    std::filesystem::resize_file(path, 31);
                } else {
                    Require(AppendFramePrefix(path, start, kTailTorn, kNextValues, 12),
                            "invalid history also contains a candidate incomplete tail");
                }
            }
        }
        ExpectUnchangedCorruption(fixture, Options());
    }
}

void TestScanToRepairRevalidation() {
    for (const std::string_view change : {"identity", "length", "header"}) {
        auto fixture = Prepare("revalidate-" + std::string(change));
        const auto instance = FixtureInstance(fixture);
        const auto path = WriteWalHeader(fixture, "tail.log", instance, 1);
        Require(AppendFramePrefix(path, 1, kTailTorn, kFirstValues, 12), "create repairable tail");
        const auto before_scan = WalContents(fixture);
        const auto options = Options();
        const auto scanned = ScanWalDirectory(path.parent_path(), instance, fixture.dimension,
                                              0, options.wal);
        const auto* report = std::get_if<RecoveryReport>(&scanned);
        Require(report && report->latest &&
                report->latest->valid_bytes == wal_detail::kWalHeaderBytes &&
                report->latest->file.scanned_bytes == before_scan.front().second.size(),
                "scan returns the validated target and exact truncation offset");
        Require(WalContents(fixture) == before_scan, "scan alone never changes the WAL");
        if (!report || !report->latest) return;
        if (change == "identity") {
            auto displaced = path;
            displaced.replace_extension(".displaced");
            std::filesystem::rename(path, displaced);
            std::filesystem::copy_file(displaced, path);
        } else if (change == "length") {
            std::ofstream(path, std::ios::app | std::ios::binary).put('A');
        } else {
            const auto bytes = ReadBytes(path);
            std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
            file.put(static_cast<char>(bytes.front() ^ 1));
        }
        const auto after_change = WalContents(fixture);
        WalFaultScope injection(NativeFault::none);
        const auto& plan = *report->latest;
        const auto repaired = wal_detail::FinishWalRecovery(plan.file, plan.valid_bytes, plan.header,
                                                             fixture.dimension, options.wal);
        Require(AsError(repaired) && AsError(repaired)->code == ErrorCode::io_error &&
                AsError(repaired)->system_error == ERROR_INVALID_DATA &&
                g_wal_truncates == 0 && g_wal_flushes == 0 && WalContents(fixture) == after_change,
                "repair rejects a changed identity, size, or header before writing");
    }
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

// An empty delta has nothing to flush: the checkpoint must say so by keeping
// the sequence it already had, not by inventing a new generation.
void TestEmptyCheckpointIsANoop() {
    auto fixture = Prepare("checkpoint");
    auto engine = OpenFixture(fixture, Options());
    if (!engine) return;
    const auto before = engine->Stats();
    const auto receipt = engine->Checkpoint(std::chrono::milliseconds(1000));
    Require(std::holds_alternative<CheckpointReceipt>(receipt),
            "an empty checkpoint succeeds instead of failing");
    if (std::holds_alternative<CheckpointReceipt>(receipt) &&
        std::holds_alternative<EngineStats>(before)) {
        const auto& value = std::get<CheckpointReceipt>(receipt);
        const auto& stats = std::get<EngineStats>(before);
        Require(value.checkpoint_sequence == stats.checkpoint_sequence,
                "an empty checkpoint does not move the checkpoint sequence");
        Require(value.target_sequence == stats.published_sequence,
                "an empty checkpoint reports the sequence it saw");
    }
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
    if (argc == 2 && std::string_view(argv[1]) == "--prepare-recovery-study")
        return PrepareRecoveryStudy();
    if (argc == 3 && std::string_view(argv[1]) == "--measure-recovery-study")
        return MeasureRecoveryStudy(argv[2]);
    if (argc >= 4 && std::string(argv[1]) == "--crash-child")
        return CrashChild(argv[2], std::atoi(argv[3]));
    if (argc == 3 && std::string(argv[1]) == "--tail-recovery-child")
        return TailRecoveryChild(argv[2]);
    if (argc == 3 && std::string(argv[1]) == "--truncate-child")
        return RecoveryFaultChild(argv[2], NativeFault::kill_on_recovery_flush);
    if (argc == 3 && std::string(argv[1]) == "--unreplied-child")
        return RecoveryFaultChild(argv[2], NativeFault::kill_after_frame_flush);

    TestConfirmThenVisible();
    TestVersionsAndConflict();
    TestSnapshotIsolation();
    TestConcurrentQueriesAndWrites();
    TestRecoveryReplaysConfirmedWrites();
    TestRecoveryLiveCountCountsIds();
    TestTornTailThenConfirmedWriteSurvivesAnotherOpen();
    TestEmptyLatestWalIsAdopted();
    TestRotatedEmptyWalIsAdopted();
    TestWriteFaultsAtEngineBoundary();
    TestWriteAdmissionTimeoutAndClose();
    TestViewAdmissionHonorsDeadlines();
    TestRecoveryIoFailuresAndRetry();
    TestCompleteUnrepliedFrameReplays();
    TestNewWalPublicationFailures();
    TestCorruptFrameIsRefused();
    TestHistoricalShortTailIsRefused();
    TestInvalidHistoryRemainsUnchanged();
    TestScanToRepairRevalidation();
    TestRefusals();
    TestDeltaCapacityIsEnforced();
    TestWalHistoryWorkspaceLimit();
    TestRestartReclaimsOnlyCoveredWal();
    TestEmptyCheckpointIsANoop();
    TestProcessCrashRecovery();

    std::printf("dynamic-engine: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d dynamic engine checks failed\n", g_failures);
        return 1;
    }
    return 0;
}
