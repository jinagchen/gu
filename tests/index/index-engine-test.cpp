// Stage 4 B checks: manifest codec, scratch derivation, first build, open,
// search, point reads and the documented error paths.

#include "agent_io/engine.hpp"
#include "agent_io/index.hpp"

#include "../../src/index/builder.hpp"
#include "../../src/index/digest.hpp"
#include "../../src/index/format.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
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

template <class T>
const T& Expect(const Result<T>& result, const char* message) {
    Require(std::holds_alternative<T>(result), message);
    static T fallback{};
    if (!std::holds_alternative<T>(result)) return fallback;
    return std::get<T>(result);
}

ErrorCode CodeOf(const Result<Success>& result) {
    return std::holds_alternative<Error>(result) ? std::get<Error>(result).code :
                                                   ErrorCode::closed;
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

void MakeFixture(const std::filesystem::path& vectors, const std::filesystem::path& ids,
                 std::uint32_t count, std::uint32_t dimension, std::uint64_t seed,
                 std::vector<float>& values, std::vector<VectorId>& identifiers) {
    const std::uint32_t clusters = 16;
    std::vector<float> centers(static_cast<std::size_t>(clusters) * dimension);
    for (auto& value : centers) value = static_cast<float>(NextUnit(seed) * 4.0 - 2.0);
    values.assign(static_cast<std::size_t>(count) * dimension, 0.0f);
    identifiers.resize(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint32_t cluster = index % clusters;
        for (std::uint32_t component = 0; component < dimension; ++component) {
            const double noise = (NextUnit(seed) - 0.5) * 0.05;
            values[static_cast<std::size_t>(index) * dimension + component] =
                centers[static_cast<std::size_t>(cluster) * dimension + component] +
                static_cast<float>(noise);
        }
        identifiers[index] = 1000u + index * 7u;
    }
    std::ofstream vector_file(vectors, std::ios::binary | std::ios::trunc);
    vector_file.write(reinterpret_cast<const char*>(values.data()),
                      static_cast<std::streamsize>(values.size() * sizeof(float)));
    vector_file.close();
    std::ofstream id_file(ids, std::ios::binary | std::ios::trunc);
    id_file.write(reinterpret_cast<const char*>(identifiers.data()),
                  static_cast<std::streamsize>(identifiers.size() * sizeof(VectorId)));
    id_file.close();
}

double Distance(const float* left, const float* right, std::uint32_t dimension) {
    double total = 0.0;
    for (std::uint32_t index = 0; index < dimension; ++index) {
        const double delta = static_cast<double>(left[index]) - static_cast<double>(right[index]);
        total += delta * delta;
    }
    return total;
}

void TestManifest() {
    Manifest manifest;
    manifest.instance_id = NewInstanceId();
    manifest.generation = 1;
    manifest.checkpoint_sequence = 0;
    manifest.dimension = 8;
    manifest.block_bytes = 4096;
    manifest.layout = NodeRecordLayout{8, 16, 4096, 0, 32, 100};
    Require(std::holds_alternative<Success>(
        SetManifestText(manifest.adapter.name, "agent_io_native_graph")), "adapter name");
    Require(std::holds_alternative<Success>(
        SetManifestText(manifest.adapter.commit, "0000000000000000000000000000000000000000")),
        "adapter commit");
    Require(std::holds_alternative<Success>(
        SetManifestText(manifest.adapter.native_version, "1.0.0")), "adapter version");
    Require(std::holds_alternative<Success>(
        SetManifestText(manifest.adapter.parameters, "R16")), "adapter parameters");
    manifest.segment_count = 1;
    auto& entry = manifest.segments[0];
    entry.id = 1;
    entry.uuid = NewInstanceId();
    entry.generation = 1;
    entry.kind = SegmentKind::base;
    entry.covered_sequence = 0;
    entry.node_count = 10;
    entry.file_bytes = 40960;
    entry.content_sha256 = Digest{};
    Require(std::holds_alternative<Success>(SetManifestText(
        entry.metadata.path, "segments/" + FormatUuid(entry.uuid) + "/segment.json")),
        "metadata path");
    entry.metadata.logical_bytes = 512;
    entry.metadata.physical_bytes = 512;
    manifest.live_count = 10;

    const auto size = ManifestBytes(manifest);
    Require(std::holds_alternative<std::uint64_t>(size), "manifest bytes");
    const auto bytes = std::get<std::uint64_t>(size);
    std::vector<std::byte> buffer(static_cast<std::size_t>(bytes));
    const auto encoded = EncodeManifest(manifest, buffer);
    Require(std::holds_alternative<std::uint64_t>(encoded) &&
            std::get<std::uint64_t>(encoded) == bytes, "manifest encode length");
    const auto decoded = DecodeManifest(buffer);
    Require(std::holds_alternative<Manifest>(decoded), "manifest round trip");
    if (std::holds_alternative<Manifest>(decoded)) {
        const auto& round = std::get<Manifest>(decoded);
        Require(round.generation == manifest.generation, "generation round trip");
        Require(round.segment_count == 1 && round.segments[0].node_count == 10,
                "segment round trip");
        Require(ManifestTextView(round.adapter.name) == "agent_io_native_graph",
                "adapter round trip");
        Require(round.layout.row_bytes == 4096, "layout round trip");
    }

    // A truncated buffer and an unknown field must both be rejected.
    auto short_buffer = buffer;
    short_buffer.pop_back();
    Require(std::holds_alternative<Error>(DecodeManifest(short_buffer)),
            "truncated manifest is corruption");
    std::string text(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    const auto injected = text.insert(text.size() - 1, ",\"extra\":1");
    const auto injected_bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(injected.data()), injected.size());
    Require(std::holds_alternative<Error>(DecodeManifest(injected_bytes)),
            "unknown manifest field is rejected");
    std::vector<std::byte> tiny(16);
    Require(std::holds_alternative<Error>(EncodeManifest(manifest, tiny)),
            "small output buffer is rejected");

    Manifest broken = manifest;
    broken.layout.row_bytes = 4000;
    Require(std::holds_alternative<Error>(ManifestBytes(broken)),
            "row_bytes must be a block multiple");
    broken = manifest;
    broken.segments[1].id = 1;
    broken.segment_count = 2;
    Require(std::holds_alternative<Error>(ManifestBytes(broken)), "duplicate segment id");
}

void TestScratch() {
    ScratchRequest request;
    request.dimension = 8;
    request.budget.total_limit = 128;
    request.budget.per_segment_limit = 64;
    request.budget.delta_row_limit = 12288;
    request.budget.workspace_bytes = 1u << 20;
    request.segment_count = 1;
    request.max_degree = 32;
    request.copy_buffer_blocks = 33;
    request.block_bytes = 4096;
    request.pq_scratch_bytes = std::uint64_t{0};
    request.navigation_scratch_bytes = 65536;
    const auto plan = ComputeScratchPlan(request);
    Require(std::holds_alternative<ScratchPlan>(plan), "scratch plan");
    if (std::holds_alternative<ScratchPlan>(plan)) {
        const auto& value = std::get<ScratchPlan>(plan);
        Require(value.total_bytes % 64 == 0, "scratch total is 64-byte aligned");
        Require(value.total_bytes > value.io_buffer_bytes, "scratch includes copy buffers");
    }
    auto tight = request;
    tight.budget.workspace_bytes = 1024;
    Require(std::holds_alternative<Error>(ComputeScratchPlan(tight)),
            "scratch larger than workspace is rejected");
    auto unknown = request;
    unknown.pq_scratch_bytes.reset();
    Require(std::holds_alternative<Error>(ComputeScratchPlan(unknown)),
            "missing upstream scratch is unsupported");
    auto oversized = request;
    oversized.segment_count = 32;
    Require(std::holds_alternative<Error>(ComputeScratchPlan(oversized)),
            "segment count beyond the view capacity is rejected");
}

struct Fixture {
    std::filesystem::path root;
    std::filesystem::path vectors;
    std::filesystem::path ids;
    std::filesystem::path index;
    std::filesystem::path staging;
    std::vector<float> values;
    std::vector<VectorId> identifiers;
    std::uint32_t dimension = 8;
    std::uint32_t count = 256;
};

Fixture Prepare(const std::string& name, std::uint64_t seed) {
    Fixture fixture;
    fixture.root = std::filesystem::temp_directory_path() /
        ("agent_io_index_test_" + name + "_" + std::to_string(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(fixture.root, error);
    std::filesystem::create_directories(fixture.root, error);
    fixture.vectors = fixture.root / "vectors.f32";
    fixture.ids = fixture.root / "ids.u64";
    fixture.index = fixture.root / "index";
    fixture.staging = fixture.root / "index.staging";
    MakeFixture(fixture.vectors, fixture.ids, fixture.count, fixture.dimension, seed,
                fixture.values, fixture.identifiers);
    return fixture;
}

FirstBuildRequest BuildRequestFor(const Fixture& fixture, std::uint32_t degree = 16) {
    FirstBuildRequest request;
    request.vectors_path = fixture.vectors;
    request.ids_path = fixture.ids;
    request.dimension = fixture.dimension;
    request.max_degree = degree;
    request.build_beam = 4;
    request.vector_count = fixture.count;
    request.staging_directory = fixture.staging;
    request.output_directory = fixture.index;
    request.build_id = "index-engine-test";
    request.native_parameters = "R16 exact-knn baseline";
    // The manifest records the base library version next to the adapter's, so a
    // first build without one is a configuration error, not a default.
    request.native_version = "test-native-1.0";
    request.min_free_disk_bytes = 0;
    return request;
}

MemoryBudget TestBudget() {
    MemoryBudget budget;
    budget.total_bytes = 96u << 20;
    budget.cache_bytes = 8u << 20;
    budget.delta_bytes = 8u << 20;
    budget.metadata_bytes = 8u << 20;
    budget.workspace_bytes = 64u << 20;
    budget.reserve_bytes = 8u << 20;
    return budget;
}

void TestBuildAndSearch(Fixture& fixture) {
    // A missing native_version must be refused before anything is written: the
    // manifest has no way to express "unknown base library".
    auto unversioned = BuildRequestFor(fixture, 32);
    unversioned.native_version.clear();
    const auto refused = RunFirstBuild(unversioned);
    Require(std::holds_alternative<Error>(refused) &&
            std::get<Error>(refused).code == ErrorCode::invalid_argument,
            "a first build without native_version is refused");
    Require(!std::filesystem::exists(fixture.staging), "a refused build creates no staging");

    auto request = BuildRequestFor(fixture, 32);
    const auto outcome = RunFirstBuild(request);
    Require(std::holds_alternative<BuildOutcome>(outcome), "first build succeeds");
    if (!std::holds_alternative<BuildOutcome>(outcome)) {
        if (std::holds_alternative<Error>(outcome))
            std::fprintf(stderr, "  build error: %s\n",
                         std::get<Error>(outcome).message.c_str());
        return;
    }
    const auto& built = std::get<BuildOutcome>(outcome);
    Require(built.published, "first build publishes CURRENT");
    Require(std::filesystem::exists(fixture.index / "CURRENT"), "CURRENT exists");
    Require(!std::filesystem::exists(fixture.staging), "staging is moved into place");

    const auto second = RunFirstBuild(request);
    Require(std::holds_alternative<Error>(second) &&
            std::get<Error>(second).code == ErrorCode::already_exists,
            "a first build refuses to overwrite an index");

    EngineOptions options;
    options.memory = TestBudget();
    options.block_bytes = 4096;
    options.max_queries = 4;
    options.max_inflight_reads = 64;
    options.direct_reads = true;
    auto engine_result = Engine::Open(fixture.index, options);
    Require(std::holds_alternative<std::unique_ptr<Engine>>(engine_result), "engine opens");
    if (!std::holds_alternative<std::unique_ptr<Engine>>(engine_result)) {
        if (std::holds_alternative<Error>(engine_result))
            std::fprintf(stderr, "  open error: %s\n",
                         std::get<Error>(engine_result).message.c_str());
        return;
    }
    auto engine = std::get<std::unique_ptr<Engine>>(std::move(engine_result));

    const auto stats = engine->Stats();
    Require(std::holds_alternative<EngineStats>(stats), "engine stats");
    if (std::holds_alternative<EngineStats>(stats))
        Require(std::get<EngineStats>(stats).live_vectors == fixture.count, "live vector count");

    // Exact ground truth for a handful of queries near real points.
    std::uint64_t seed = 99;
    std::uint32_t hits = 0, total = 0;
    for (int round = 0; round < 8; ++round) {
        const std::uint32_t target = NextRandom(seed) % fixture.count;
        std::vector<float> query(fixture.dimension);
        for (std::uint32_t component = 0; component < fixture.dimension; ++component) {
            query[component] =
                fixture.values[static_cast<std::size_t>(target) * fixture.dimension + component] +
                static_cast<float>((NextUnit(seed) - 0.5) * 0.01);
        }
        std::vector<std::pair<double, VectorId>> distances;
        distances.reserve(fixture.count);
        for (std::uint32_t index = 0; index < fixture.count; ++index)
            distances.emplace_back(Distance(query.data(),
                fixture.values.data() + static_cast<std::size_t>(index) * fixture.dimension,
                fixture.dimension), fixture.identifiers[index]);
        std::partial_sort(distances.begin(), distances.begin() + 10, distances.end());
        SearchOptions search;
        search.k = 10;
        search.candidate_limit = 64;
        search.timeout = std::chrono::milliseconds(5000);
        const auto result = engine->Search(query, search);
        Require(std::holds_alternative<SearchResult>(result), "search succeeds");
        if (!std::holds_alternative<SearchResult>(result)) {
            if (std::holds_alternative<Error>(result))
                std::fprintf(stderr, "  search error: %s\n",
                             std::get<Error>(result).message.c_str());
            continue;
        }
        const auto& found = std::get<SearchResult>(result);
        Require(found.neighbors.size() == 10, "search returns K neighbours");
        Require(found.snapshot_sequence == 0, "static snapshot is zero");
        std::vector<VectorId> truth;
        for (int index = 0; index < 10; ++index) truth.push_back(distances[index].second);
        for (const auto& neighbour : found.neighbors) {
            ++total;
            hits += std::find(truth.begin(), truth.end(), neighbour.id) != truth.end() ? 1 : 0;
        }
        for (std::size_t index = 1; index < found.neighbors.size(); ++index)
            Require(found.neighbors[index - 1].squared_distance <=
                    found.neighbors[index].squared_distance + 1e-9,
                    "neighbours are sorted by exact distance");
    }
    if (total) std::printf("recall@10=%.3f (%u/%u)\n",
        static_cast<double>(hits) / total, hits, total);
    Require(total > 0 && hits * 100 >= total * 90, "small-fixture recall is at least 0.90");

    const auto stored = engine->Get(fixture.identifiers[3]);
    Require(std::holds_alternative<StoredVector>(stored), "Get returns a stored vector");
    if (std::holds_alternative<StoredVector>(stored)) {
        const auto& vector = std::get<StoredVector>(stored);
        Require(vector.id == fixture.identifiers[3] && vector.version == 0, "Get identity");
        Require(vector.values.size() == fixture.dimension, "Get dimension");
    }
    const auto missing = engine->Get(424242);
    Require(std::holds_alternative<Error>(missing) &&
            std::get<Error>(missing).code == ErrorCode::not_found, "Get unknown id");

    std::vector<float> wrong_dimension(3, 0.0f);
    SearchOptions search;
    search.k = 10;
    search.candidate_limit = 64;
    const auto bad_query = engine->Search(wrong_dimension, search);
    Require(std::holds_alternative<Error>(bad_query) &&
            std::get<Error>(bad_query).code == ErrorCode::invalid_argument,
            "dimension mismatch is rejected");
    SearchOptions bad_k;
    bad_k.k = 0;
    bad_k.candidate_limit = 64;
    std::vector<float> query(fixture.dimension, 0.0f);
    Require(std::holds_alternative<Error>(engine->Search(query, bad_k)), "K zero is rejected");
    SearchOptions over;
    over.k = 100;
    over.candidate_limit = 64;
    Require(std::holds_alternative<Error>(engine->Search(query, over)),
            "K above the candidate limit is rejected");

    // The baseline is now writable: a durable write appended to a static index
    // must be acknowledged and survive as a new version, not be rejected.
    const auto appended = engine->Insert(1, query, WriteOptions{});
    Require(std::holds_alternative<WriteReceipt>(appended),
            "the baseline accepts a durable write");
    if (std::holds_alternative<WriteReceipt>(appended))
        Require(std::get<WriteReceipt>(appended).sequence == 1,
                "the first durable write takes sequence 1");
    Require(std::holds_alternative<StoredVector>(engine->Get(1)),
            "the durable write is readable on the baseline");
    // Stage 7 makes an explicit checkpoint work even without background
    // maintenance: the write above moves from the delta into a new segment.
    const auto checkpointed = engine->Checkpoint(std::chrono::milliseconds(5000));
    Require(std::holds_alternative<CheckpointReceipt>(checkpointed),
            "an explicit checkpoint flushes the delta");
    if (std::holds_alternative<CheckpointReceipt>(checkpointed))
        Require(std::get<CheckpointReceipt>(checkpointed).checkpoint_sequence == 1,
                "the checkpoint covers the acknowledged write");
    Require(std::holds_alternative<StoredVector>(engine->Get(1)),
            "the write stays readable across a checkpoint");

    // A second engine on the same directory must fail through the LOCK file.
    auto locked = Engine::Open(fixture.index, options);
    Require(std::holds_alternative<Error>(locked), "a second instance cannot lock the index");
    if (std::holds_alternative<Error>(locked))
        Require(std::get<Error>(locked).system_error != 0, "lock failure reports the OS error");

    Require(std::holds_alternative<Success>(engine->Close()), "engine closes");
    Require(std::holds_alternative<Error>(engine->Search(query, search)), "closed engine rejects");
}

void TestBuildRejections() {
    auto fixture = Prepare("reject", 7);
    auto request = BuildRequestFor(fixture, 32);

    auto duplicate = request;
    duplicate.ids_path = fixture.root / "duplicate.u64";
    {
        std::vector<VectorId> ids(fixture.count, 5);
        std::ofstream out(duplicate.ids_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(ids.data()),
                  static_cast<std::streamsize>(ids.size() * sizeof(VectorId)));
    }
    const auto duplicate_result = RunFirstBuild(duplicate);
    Require(std::holds_alternative<Error>(duplicate_result) &&
            std::get<Error>(duplicate_result).code == ErrorCode::invalid_argument,
            "duplicate IDs are rejected");

    auto short_vectors = request;
    short_vectors.vectors_path = fixture.root / "short.f32";
    {
        std::ofstream out(short_vectors.vectors_path, std::ios::binary | std::ios::trunc);
        const float value = 0.0f;
        out.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }
    Require(std::holds_alternative<Error>(RunFirstBuild(short_vectors)),
            "a short vector file is rejected");

    auto nested = request;
    nested.staging_directory = fixture.index / "staging";
    Require(std::holds_alternative<Error>(RunFirstBuild(nested)),
            "staging inside the output is rejected");

    auto bad_hash = request;
    bad_hash.vectors_sha256 = std::string(64, '0');
    Require(std::holds_alternative<Error>(RunFirstBuild(bad_hash)),
            "input hash mismatch is rejected");

    std::error_code error;
    std::filesystem::remove_all(fixture.root, error);
}

}  // namespace

int main() {
    TestManifest();
    TestScratch();
    auto fixture = Prepare("build", 20261121);
    TestBuildAndSearch(fixture);
    std::error_code error;
    std::filesystem::remove_all(fixture.root, error);
    TestBuildRejections();
    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
