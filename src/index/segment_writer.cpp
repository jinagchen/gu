#include "segment_writer.hpp"

#include "digest.hpp"
#include "format.hpp"
#include "graph.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <numeric>
#include <unordered_set>

namespace agent_io {
namespace {

Error Failure(ErrorCode code, std::string message, int system_error = 0) {
    return Error{code, std::move(message), system_error};
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
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool flushed = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    return flushed;
}

}  // namespace

Result<SegmentArtifact> WriteSegment(const SegmentWriteRequest& request) {
    if (request.dimension == 0) return Failure(ErrorCode::invalid_argument,
                                               "Segment dimension must be positive");
    if (request.records.empty())
        return Failure(ErrorCode::invalid_argument, "A segment needs at least one record");
    if (request.records.size() > 0xffffffffull)
        return Failure(ErrorCode::invalid_argument, "Segment record count exceeds node ids");
    if (request.staging_directory.empty())
        return Failure(ErrorCode::invalid_argument, "Segment write needs a staging directory");
    const auto layout_status = ValidateLayout(request.layout, request.dimension,
                                              request.block_bytes);
    if (const auto* error = std::get_if<Error>(&layout_status)) return *error;
    for (const auto& record : request.records) {
        if (record.id == 0)
            return Failure(ErrorCode::invalid_argument, "Logical ID zero is not allowed");
        if (record.values.size() != request.dimension)
            return Failure(ErrorCode::invalid_argument, "Segment vector dimension mismatch");
    }

    const std::uint64_t count = request.records.size();
    std::vector<float> flat(static_cast<std::size_t>(count) * request.dimension);
    for (std::size_t index = 0; index < request.records.size(); ++index)
        std::memcpy(flat.data() + index * request.dimension, request.records[index].values.data(),
                    static_cast<std::size_t>(request.dimension) * sizeof(float));

    auto adjacency = BuildAdjacency(flat, count, request.dimension, request.layout.max_degree);
    if (const auto* error = std::get_if<Error>(&adjacency)) return *error;
    const auto& edges = std::get<GraphAdjacency>(adjacency);

    std::error_code path_error;
    std::filesystem::create_directories(request.staging_directory, path_error);
    if (path_error)
        return Failure(ErrorCode::io_error, "Cannot create the segment staging directory",
                       static_cast<int>(path_error.value()));

    const auto graph_path = request.staging_directory / "graph.bin";
    const auto written = WriteGraphRows(graph_path, request.layout, flat, edges,
        [&request](std::size_t node) {
            const auto& record = request.records[node];
            return std::pair{record.id, record.version};
        });
    if (const auto* error = std::get_if<Error>(&written)) return *error;
    const auto graph_bytes = std::get<std::uint64_t>(written);
    if (!FlushPath(graph_path))
        return Failure(ErrorCode::io_error, "Cannot flush the graph segment",
                       static_cast<int>(GetLastError()));

    std::vector<std::uint32_t> order(static_cast<std::size_t>(count));
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&request](std::uint32_t left, std::uint32_t right) {
        return request.records[left].id < request.records[right].id;
    });
    // The version directory must be strictly sorted by logical ID, because the
    // reader's sparse index assumes it. Duplicates would make Find ambiguous.
    for (std::size_t index = 1; index < order.size(); ++index) {
        if (request.records[order[index - 1]].id == request.records[order[index]].id)
            return Failure(ErrorCode::invalid_argument,
                           "Segment records contain a duplicate logical ID");
    }
    const std::uint64_t version_logical = count * kVersionRecordBytes;
    const std::uint64_t version_physical = AlignUp(version_logical, request.block_bytes);
    std::vector<std::byte> version_bytes(static_cast<std::size_t>(version_physical), std::byte{0});
    for (std::uint64_t index = 0; index < count; ++index) {
        VersionRecord record;
        record.id = request.records[order[static_cast<std::size_t>(index)]].id;
        record.version = request.records[order[static_cast<std::size_t>(index)]].version;
        record.node = order[static_cast<std::size_t>(index)];
        const auto encoded = EncodeVersionRecord(
            record, std::span<std::byte>(
                        version_bytes.data() + index * kVersionRecordBytes, kVersionRecordBytes));
        if (const auto* error = std::get_if<Error>(&encoded)) return *error;
    }
    const auto versions_path = request.staging_directory / "versions.bin";
    if (!WriteWhole(versions_path, version_bytes))
        return Failure(ErrorCode::io_error, "Cannot write the version directory");
    if (!FlushPath(versions_path))
        return Failure(ErrorCode::io_error, "Cannot flush the version directory",
                       static_cast<int>(GetLastError()));

    const auto graph_digest = HashFile(graph_path);
    if (const auto* error = std::get_if<Error>(&graph_digest)) return *error;
    const auto versions_digest = HashFile(versions_path);
    if (const auto* error = std::get_if<Error>(&versions_digest)) return *error;

    SegmentMetadata metadata;
    metadata.uuid = NewInstanceId();
    metadata.dimension = request.dimension;
    metadata.block_bytes = request.block_bytes;
    metadata.row_bytes = request.layout.row_bytes;
    metadata.layout = request.layout;
    metadata.node_count = count;
    metadata.builder = request.builder;
    metadata.builder_version = request.builder_version;
    metadata.parameters = request.parameters;
    metadata.version_directory_file = "versions.bin";
    metadata.version_entry_count = count;
    metadata.files.push_back(SegmentFileRecord{"graph.bin", graph_bytes, graph_bytes,
                                               std::get<Digest>(graph_digest)});
    metadata.files.push_back(SegmentFileRecord{"versions.bin", version_logical, version_physical,
                                               std::get<Digest>(versions_digest)});
    const auto metadata_text = EncodeSegmentMetadata(metadata);
    if (const auto* error = std::get_if<Error>(&metadata_text)) return *error;
    const std::string& encoded_metadata = std::get<std::string>(metadata_text);
    const auto metadata_path = request.staging_directory / "segment.json";
    if (!WriteWhole(metadata_path,
                    std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(encoded_metadata.data()),
                        encoded_metadata.size())))
        return Failure(ErrorCode::io_error, "Cannot write segment.json");
    if (!FlushPath(metadata_path))
        return Failure(ErrorCode::io_error, "Cannot flush segment.json",
                       static_cast<int>(GetLastError()));
    const auto metadata_digest = HashFile(metadata_path);
    if (const auto* error = std::get_if<Error>(&metadata_digest)) return *error;

    SegmentArtifact artifact;
    artifact.uuid = metadata.uuid;
    artifact.node_count = count;
    artifact.graph_bytes = graph_bytes;
    artifact.graph_sha256 = std::get<Digest>(graph_digest);
    artifact.version_logical_bytes = version_logical;
    artifact.version_physical_bytes = version_physical;
    artifact.version_sha256 = std::get<Digest>(versions_digest);
    artifact.metadata_bytes = encoded_metadata.size();
    artifact.metadata_sha256 = std::get<Digest>(metadata_digest);
    artifact.staged_bytes = graph_bytes + version_physical + encoded_metadata.size();
    return artifact;
}

}  // namespace agent_io
