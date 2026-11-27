#include "builder.hpp"

#include "digest.hpp"
#include "format.hpp"
#include "graph.hpp"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <queue>
#include <unordered_set>

namespace agent_io {
namespace {

constexpr std::uint64_t kMaxBuildWork = 4'000'000'000ull;  // node pairs x dimension
constexpr std::uint32_t kDefaultBlockBytes = 4096;

Error Invalid(std::string message) {
    return Error{ErrorCode::invalid_argument, std::move(message)};
}

Error Failure(ErrorCode code, std::string message, int system_error = 0) {
    Error error{code, std::move(message)};
    error.system_error = system_error;
    return error;
}

bool ReadWhole(const std::filesystem::path& path, std::vector<std::byte>& output) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const auto size = input.tellg();
    if (size < 0) return false;
    output.assign(static_cast<std::size_t>(size), std::byte{0});
    input.seekg(0);
    if (size > 0) input.read(reinterpret_cast<char*>(output.data()), size);
    return static_cast<bool>(input);
}

bool WriteWhole(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    if (!bytes.empty())
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    output.flush();
    return static_cast<bool>(output);
}

bool FlushPath(const std::filesystem::path& path) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool ok = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    return ok;
}

std::uint64_t FreeBytes(const std::filesystem::path& path) {
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(path.c_str(), &available, nullptr, nullptr)) return 0;
    return available.QuadPart;
}

std::uint64_t CommitPeakBytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters)))
        return 0;
    return static_cast<std::uint64_t>(counters.PeakPagefileUsage);
}

bool SameOrNested(const std::filesystem::path& left, const std::filesystem::path& right) {
    const auto normal_left = std::filesystem::weakly_canonical(left);
    const auto normal_right = std::filesystem::weakly_canonical(right);
    auto prefix = [](const std::filesystem::path& outer, const std::filesystem::path& inner) {
        auto outer_it = outer.begin();
        auto inner_it = inner.begin();
        for (; outer_it != outer.end(); ++outer_it, ++inner_it) {
            if (inner_it == inner.end() || *inner_it != *outer_it) return false;
        }
        return true;
    };
    return prefix(normal_left, normal_right) || prefix(normal_right, normal_left);
}

}  // namespace

Result<BuildOutcome> RunFirstBuild(const FirstBuildRequest& request) {
    if (request.dimension == 0 || request.dimension > 65536)
        return Invalid("Build dimension is out of range");
    if (request.max_degree == 0 || request.max_degree > 512)
        return Invalid("Build max_degree is out of range");
    if (request.vector_count == 0) return Invalid("Build needs at least one vector");
    if (request.vector_count > 0xffffffffull)
        return Invalid("Vector count exceeds the native node id width");
    if (request.staging_directory.empty() || request.output_directory.empty())
        return Invalid("Build needs staging and output directories");
    if (SameOrNested(request.staging_directory, request.output_directory))
        return Invalid("Staging and output directories must differ and must not nest");
    if (!IsAsciiPath(request.vectors_path) || !IsAsciiPath(request.ids_path))
        return Invalid("Input paths must be ASCII");
    // The manifest records both the adapter and the native library it sits on.
    // Manifest text cannot be empty, so a missing version has to be refused
    // here rather than discovered while the manifest is being encoded.
    if (request.adapter_name.empty() || request.adapter_version.empty())
        return Invalid("Build needs adapter_name and adapter_version");
    if (request.native_version.empty())
        return Invalid("Build needs native_version: the manifest records it");

    // 1. Input validation happens before any staging directory exists.
    std::vector<std::byte> vector_bytes;
    if (!ReadWhole(request.vectors_path, vector_bytes))
        return Failure(ErrorCode::io_error, "Cannot read the vector file");
    std::vector<std::byte> id_bytes;
    if (!ReadWhole(request.ids_path, id_bytes))
        return Failure(ErrorCode::io_error, "Cannot read the ID file");
    const std::uint64_t expected_vectors = request.vector_count * request.dimension * 4;
    if (vector_bytes.size() != expected_vectors)
        return Invalid("Vector file length does not match vector_count x dimension x 4");
    if (id_bytes.size() != request.vector_count * 8)
        return Invalid("ID file length does not match vector_count x 8");

    if (!request.vectors_sha256.empty()) {
        const auto digest = HashFile(request.vectors_path);
        if (const auto* error = std::get_if<Error>(&digest)) return *error;
        if (FormatDigest(std::get<Digest>(digest)) != request.vectors_sha256)
            return Failure(ErrorCode::corruption, "Vector file hash does not match the manifest");
    }
    if (!request.ids_sha256.empty()) {
        const auto digest = HashFile(request.ids_path);
        if (const auto* error = std::get_if<Error>(&digest)) return *error;
        if (FormatDigest(std::get<Digest>(digest)) != request.ids_sha256)
            return Failure(ErrorCode::corruption, "ID file hash does not match the manifest");
    }

    std::vector<float> vectors(static_cast<std::size_t>(request.vector_count) * request.dimension);
    std::memcpy(vectors.data(), vector_bytes.data(), vector_bytes.size());
    std::vector<VectorId> ids(static_cast<std::size_t>(request.vector_count));
    std::memcpy(ids.data(), id_bytes.data(), id_bytes.size());
    std::unordered_set<VectorId> unique_ids;
    unique_ids.reserve(ids.size());
    for (const VectorId id : ids) {
        if (id == 0) return Invalid("Logical ID zero is not allowed");
        if (!unique_ids.insert(id).second) return Invalid("Duplicate logical ID in the input");
    }
    for (const float value : vectors)
        if (!std::isfinite(value)) return Failure(ErrorCode::corruption,
                                                  "Vector payload contains a non-finite value");

    std::error_code path_error;
    if (std::filesystem::exists(request.output_directory / "CURRENT", path_error))
        return Failure(ErrorCode::already_exists,
                       "Output already publishes an index; a first build will not overwrite it");
    const auto free_space = FreeBytes(std::filesystem::exists(request.output_directory) ?
        request.output_directory : request.output_directory.parent_path());
    const std::uint64_t output_reservation = expected_vectors + request.vector_count * 8 + (1u << 20);
    const std::uint64_t required = (std::max)(request.min_free_disk_bytes, output_reservation);
    if (free_space != 0 && free_space < required)
        return Failure(ErrorCode::resource_exhausted, "Not enough free disk for the build");
    if (std::filesystem::exists(request.staging_directory, path_error))
        return Failure(ErrorCode::already_exists, "Staging directory already exists");

    // 2. Graph construction and layout derivation.
    const std::uint32_t block_bytes = kDefaultBlockBytes;
    NodeRecordLayout layout;
    layout.dimension = request.dimension;
    layout.max_degree = request.max_degree;
    layout.vector_offset = 0;
    layout.neighbor_offset = static_cast<std::uint32_t>(
        AlignUp(static_cast<std::uint64_t>(request.dimension) * 4, 8));
    layout.metadata_offset = static_cast<std::uint32_t>(
        layout.neighbor_offset + 4 + static_cast<std::uint64_t>(request.max_degree) * 4);
    layout.row_bytes = static_cast<std::uint32_t>(AlignUp(
        static_cast<std::uint64_t>(layout.metadata_offset) + 16, block_bytes));
    const auto layout_status = ValidateLayout(layout, request.dimension, block_bytes);
    if (const auto* error = std::get_if<Error>(&layout_status)) return *error;

    auto graph = BuildAdjacency(vectors, request.vector_count, request.dimension,
                                request.max_degree);
    if (const auto* error = std::get_if<Error>(&graph)) return *error;
    const auto& adjacency = std::get<std::vector<std::vector<GraphEdge>>>(graph);

    // 3. Stage the segment files.
    std::filesystem::create_directories(request.staging_directory, path_error);
    if (path_error)
        return Failure(ErrorCode::io_error, "Cannot create the staging directory",
                       static_cast<int>(path_error.value()));
    auto cleanup_staging = [&request](bool keep) {
        if (keep) return;
        std::error_code ignored;
        std::filesystem::remove_all(request.staging_directory, ignored);
    };

    std::vector<std::byte> row(static_cast<std::size_t>(layout.row_bytes) * request.vector_count);
    for (std::uint64_t node = 0; node < request.vector_count; ++node) {
        NodeRow record;
        record.id = ids[static_cast<std::size_t>(node)];
        record.version = 0;
        for (const auto& edge : adjacency[static_cast<std::size_t>(node)])
            record.neighbors.push_back(edge.node);
        const auto encoded = EncodeNodeRow(
            record, std::span<const float>(vectors.data() + node * request.dimension,
                                           request.dimension),
            layout, std::span<std::byte>(row.data() + node * layout.row_bytes,
                                          layout.row_bytes));
        if (const auto* error = std::get_if<Error>(&encoded)) {
            cleanup_staging(false);
            return *error;
        }
    }
    const auto graph_path = request.staging_directory / "graph.bin";
    if (!WriteWhole(graph_path, row))
        return Failure(ErrorCode::io_error, "Cannot write the graph segment");
    if (!FlushPath(graph_path))
        return Failure(ErrorCode::io_error, "Cannot flush the graph segment",
                       static_cast<int>(GetLastError()));

    std::vector<std::uint32_t> order(static_cast<std::size_t>(request.vector_count));
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&ids](std::uint32_t left, std::uint32_t right) {
        return ids[left] < ids[right];
    });
    const std::uint64_t version_directory_bytes = request.vector_count * kVersionRecordBytes;
    const std::uint64_t version_physical = AlignUp(version_directory_bytes, block_bytes);
    std::vector<std::byte> version_bytes(static_cast<std::size_t>(version_physical),
                                         std::byte{0});
    for (std::uint64_t index = 0; index < request.vector_count; ++index) {
        VersionRecord record;
        record.id = ids[order[static_cast<std::size_t>(index)]];
        record.version = 0;
        record.node = order[static_cast<std::size_t>(index)];
        const auto encoded = EncodeVersionRecord(
            record, std::span<std::byte>(version_bytes.data() + index * kVersionRecordBytes,
                                          kVersionRecordBytes));
        if (const auto* error = std::get_if<Error>(&encoded)) {
            cleanup_staging(false);
            return *error;
        }
    }
    const auto versions_path = request.staging_directory / "versions.bin";
    if (!WriteWhole(versions_path, version_bytes))
        return Failure(ErrorCode::io_error, "Cannot write the version directory");
    if (!FlushPath(versions_path))
        return Failure(ErrorCode::io_error, "Cannot flush the version directory",
                       static_cast<int>(GetLastError()));

    const auto graph_digest = HashFile(graph_path);
    if (const auto* error = std::get_if<Error>(&graph_digest)) {
        cleanup_staging(false);
        return *error;
    }
    const auto versions_digest = HashFile(versions_path);
    if (const auto* error = std::get_if<Error>(&versions_digest)) {
        cleanup_staging(false);
        return *error;
    }

    SegmentMetadata metadata;
    metadata.uuid = NewInstanceId();
    metadata.dimension = request.dimension;
    metadata.block_bytes = block_bytes;
    metadata.row_bytes = layout.row_bytes;
    metadata.layout = layout;
    metadata.node_count = request.vector_count;
    metadata.builder = request.adapter_name;
    metadata.builder_version = request.adapter_version;
    metadata.parameters = request.native_parameters;
    metadata.version_directory_file = "versions.bin";
    metadata.version_entry_count = request.vector_count;
    metadata.files.push_back(SegmentFileRecord{"graph.bin", row.size(), row.size(),
                                               std::get<Digest>(graph_digest)});
    metadata.files.push_back(SegmentFileRecord{"versions.bin", version_directory_bytes,
                                               version_physical,
                                               std::get<Digest>(versions_digest)});
    const auto metadata_text = EncodeSegmentMetadata(metadata);
    if (const auto* error = std::get_if<Error>(&metadata_text)) {
        cleanup_staging(false);
        return *error;
    }
    const auto metadata_path = request.staging_directory / "segment.json";
    const std::string encoded_metadata = std::get<std::string>(metadata_text);
    if (!WriteWhole(metadata_path, std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(encoded_metadata.data()),
            encoded_metadata.size())))
        return Failure(ErrorCode::io_error, "Cannot write segment.json");
    if (!FlushPath(metadata_path))
        return Failure(ErrorCode::io_error, "Cannot flush segment.json",
                       static_cast<int>(GetLastError()));
    const auto metadata_digest = HashFile(metadata_path);
    if (const auto* error = std::get_if<Error>(&metadata_digest)) {
        cleanup_staging(false);
        return *error;
    }
    BuildOutcome outcome;
    outcome.artifacts.node_count = request.vector_count;
    outcome.artifacts.file_bytes = row.size();
    outcome.artifacts.content_sha256 = std::get<Digest>(graph_digest);
    outcome.artifacts.version_directory_sha256 = std::get<Digest>(versions_digest);
    outcome.staged_bytes = row.size() + version_physical + encoded_metadata.size();
    outcome.native_version = request.native_version;
    outcome.peak_commit_bytes = CommitPeakBytes();

    // 4. Publish: segment move, manifest, CURRENT. Nothing above touched
    //    output, so a failure here leaves any existing index untouched.
    const auto segments_directory = request.output_directory / "segments";
    const auto manifests_directory = request.output_directory / "manifests";
    std::filesystem::create_directories(segments_directory, path_error);
    if (path_error) return Failure(ErrorCode::io_error, "Cannot create the segments directory");
    std::filesystem::create_directories(manifests_directory, path_error);
    if (path_error) return Failure(ErrorCode::io_error, "Cannot create the manifests directory");
    const auto segment_directory = segments_directory / FormatUuid(metadata.uuid);
    if (std::filesystem::exists(segment_directory, path_error))
        return Failure(ErrorCode::already_exists, "Target segment directory already exists");
    if (!MoveFileExW(request.staging_directory.c_str(), segment_directory.c_str(),
                     MOVEFILE_WRITE_THROUGH))
        return Failure(ErrorCode::io_error, "Cannot move the segment into place",
                       static_cast<int>(GetLastError()));
    const auto public_metadata_path = segment_directory / "segment.json";
    outcome.artifacts.segment_file = segment_directory / "graph.bin";
    outcome.artifacts.version_directory_file = segment_directory / "versions.bin";
    outcome.artifacts.manifest_file = manifests_directory /
        ("manifest-" + std::to_string(request.generation) + ".json");

    InstanceId instance_id = NewInstanceId();
    Manifest manifest;
    manifest.instance_id = instance_id;
    manifest.generation = request.generation;
    manifest.checkpoint_sequence = request.checkpoint_sequence;
    manifest.dimension = request.dimension;
    manifest.metric = Metric::squared_l2;
    manifest.block_bytes = block_bytes;
    manifest.layout = layout;
    if (auto status = SetManifestText(manifest.adapter.name, request.adapter_name);
        !std::holds_alternative<Success>(status))
        return std::get<Error>(status);
    if (auto status = SetManifestText(manifest.adapter.commit,
        "0000000000000000000000000000000000000000"); !std::holds_alternative<Success>(status))
        return std::get<Error>(status);
    if (auto status = SetManifestText(manifest.adapter.native_version, request.native_version);
        !std::holds_alternative<Success>(status))
        return std::get<Error>(status);
    if (auto status = SetManifestText(manifest.adapter.parameters, request.native_parameters);
        !std::holds_alternative<Success>(status))
        return std::get<Error>(status);
    manifest.segment_count = 1;
    auto& entry = manifest.segments[0];
    entry.id = 1;
    entry.uuid = metadata.uuid;
    entry.generation = request.generation;
    entry.kind = SegmentKind::base;
    entry.covered_sequence = 0;
    entry.node_count = request.vector_count;
    entry.file_bytes = row.size();
    entry.content_sha256 = std::get<Digest>(graph_digest);
    if (auto status = SetManifestText(entry.metadata.path,
        "segments/" + FormatUuid(metadata.uuid) + "/segment.json");
        !std::holds_alternative<Success>(status))
        return std::get<Error>(status);
    entry.metadata.logical_bytes = encoded_metadata.size();
    entry.metadata.physical_bytes = encoded_metadata.size();
    entry.metadata.sha256 = std::get<Digest>(metadata_digest);
    manifest.live_count = request.vector_count;
    manifest.wal_replay_after = request.checkpoint_sequence;

    const auto manifest_size = ManifestBytes(manifest);
    if (const auto* error = std::get_if<Error>(&manifest_size)) return *error;
    std::vector<std::byte> manifest_bytes(static_cast<std::size_t>(
        std::get<std::uint64_t>(manifest_size)));
    const auto encoded = EncodeManifest(manifest, manifest_bytes);
    if (const auto* error = std::get_if<Error>(&encoded)) return *error;
    const auto manifest_temp = manifests_directory /
        ("manifest-" + std::to_string(request.generation) + ".json.tmp");
    if (!WriteWhole(manifest_temp, manifest_bytes))
        return Failure(ErrorCode::io_error, "Cannot write the manifest");
    if (!FlushPath(manifest_temp))
        return Failure(ErrorCode::io_error, "Cannot flush the manifest",
                       static_cast<int>(GetLastError()));
    if (!MoveFileExW(manifest_temp.c_str(), outcome.artifacts.manifest_file.c_str(),
                     MOVEFILE_WRITE_THROUGH))
        return Failure(ErrorCode::io_error, "Cannot publish the manifest",
                       static_cast<int>(GetLastError()));
    const auto manifest_digest = HashFile(outcome.artifacts.manifest_file);
    if (const auto* error = std::get_if<Error>(&manifest_digest)) return *error;

    if (request.publish_current) {
        CurrentRecord current;
        current.instance_id = instance_id;
        current.manifest = "manifests/manifest-" + std::to_string(request.generation) + ".json";
        current.manifest_sha256 = std::get<Digest>(manifest_digest);
        const auto current_text = EncodeCurrent(current);
        if (const auto* error = std::get_if<Error>(&current_text)) return *error;
        const auto& text = std::get<std::string>(current_text);
        const auto current_temp = request.output_directory / "CURRENT.tmp";
        if (!WriteWhole(current_temp, std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(text.data()), text.size())))
            return Failure(ErrorCode::io_error, "Cannot write CURRENT");
        if (!FlushPath(current_temp))
            return Failure(ErrorCode::io_error, "Cannot flush CURRENT",
                           static_cast<int>(GetLastError()));
        const auto current_path = request.output_directory / "CURRENT";
        const bool existed = std::filesystem::exists(current_path, path_error);
        bool published = false;
        if (existed) {
            const auto backup = request.output_directory /
                ("CURRENT.bak-" + FormatUuid(NewInstanceId()));
            published = ReplaceFileW(current_path.c_str(), current_temp.c_str(),
                                     backup.c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS,
                                     nullptr, nullptr) != 0;
            if (!published) {
                std::error_code ignored;
                std::filesystem::remove(backup, ignored);
            }
        } else {
            published = MoveFileExW(current_temp.c_str(), current_path.c_str(),
                                    MOVEFILE_WRITE_THROUGH) != 0;
        }
        if (!published)
            return Failure(ErrorCode::io_error,
                           "Cannot publish CURRENT; the previous index state is unchanged",
                           static_cast<int>(GetLastError()));
    }
    outcome.published = request.publish_current;
    outcome.peak_commit_bytes = CommitPeakBytes();
    outcome.artifacts.node_count = request.vector_count;
    return outcome;
}

}  // namespace agent_io

