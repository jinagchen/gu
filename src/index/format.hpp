#pragma once

// FORMAT-1 artifact plumbing shared by the builder and the reader. Everything
// here works on in-memory values only; the functions never touch the file
// system, so callers keep the Open/publish ordering explicit.

#include "agent_io/index.hpp"
#include "json.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace agent_io {

inline constexpr std::uint32_t kCurrentFormatVersion = 1;
inline constexpr std::uint32_t kSegmentFormatVersion = 1;
inline constexpr std::uint32_t kVersionRecordBytes = 32;
inline constexpr std::uint32_t kMaxBlockBytes = 1u << 20;
inline constexpr std::size_t kMaxCurrentBytes = 4096;

// Bounded UTF-8 text used by ManifestText fields.
template <std::size_t Capacity>
Result<Success> SetManifestText(ManifestText<Capacity>& target, std::string_view text) {
    if (text.empty() || text.size() > Capacity)
        return Error{ErrorCode::invalid_argument, "Manifest text length is out of range"};
    if (!index_json::IsValidUtf8(text))
        return Error{ErrorCode::invalid_argument, "Manifest text is not valid UTF-8"};
    for (const char item : text) {
        const auto raw = static_cast<unsigned char>(item);
        if (raw < 0x20 || raw == 0x7f)
            return Error{ErrorCode::invalid_argument, "Manifest text contains a control character"};
    }
    target.bytes.fill('\0');
    for (std::size_t index = 0; index < text.size(); ++index) target.bytes[index] = text[index];
    target.size = static_cast<std::uint32_t>(text.size());
    return Success{};
}

template <std::size_t Capacity>
std::string_view ManifestTextView(const ManifestText<Capacity>& text) {
    const auto length = text.size <= Capacity ? text.size : 0;
    return std::string_view(text.bytes.data(), length);
}

// CURRENT is a small JSON object naming the manifest and its content hash.
struct CurrentRecord {
    std::uint32_t format_version = kCurrentFormatVersion;
    InstanceId instance_id{};
    std::string manifest;
    Digest manifest_sha256{};
};

Result<std::string> EncodeCurrent(const CurrentRecord& current);
Result<CurrentRecord> DecodeCurrent(std::string_view text);

// segment.json: every file the reader needs, with real and padded lengths.
struct SegmentFileRecord {
    std::string path;  // relative to the segment directory
    std::uint64_t logical_bytes = 0;
    std::uint64_t physical_bytes = 0;
    Digest sha256{};
};

struct SegmentMetadata {
    std::uint32_t format_version = kSegmentFormatVersion;
    InstanceId uuid{};
    std::uint32_t dimension = 0;
    Metric metric = Metric::squared_l2;
    std::uint32_t block_bytes = 4096;
    std::uint32_t row_bytes = 0;
    NodeRecordLayout layout;
    std::uint64_t node_count = 0;
    std::string builder;
    std::string builder_version;
    std::string parameters;
    std::vector<SegmentFileRecord> files;
    std::string version_directory_file;
    std::uint64_t version_entry_count = 0;
};

Result<std::string> EncodeSegmentMetadata(const SegmentMetadata& metadata);
Result<SegmentMetadata> DecodeSegmentMetadata(std::string_view text);

// Layout validation shared by Encode/Decode and the reader.
Result<Success> ValidateLayout(const NodeRecordLayout& layout, std::uint32_t dimension,
                               std::uint32_t block_bytes);

// Little-endian version directory record (FORMAT-1 section 2).
struct VersionRecord {
    VectorId id = 0;
    Sequence version = 0;
    NativeNodeId node = 0;
};
Result<Success> EncodeVersionRecord(const VersionRecord& record, std::span<std::byte> output);
Result<VersionRecord> DecodeVersionRecord(std::span<const std::byte> input);

// Node row codec for the project-native graph segment.
struct NodeRow {
    VectorId id = 0;
    Sequence version = 0;
    std::vector<NativeNodeId> neighbors;
};
Result<NodeRow> DecodeNodeRow(std::span<const std::byte> row, const NodeRecordLayout& layout,
                              std::uint32_t dimension, std::vector<float>& vector_scratch);
Result<Success> EncodeNodeRow(const NodeRow& row, std::span<const float> values,
                              const NodeRecordLayout& layout, std::span<std::byte> output);

bool IsPowerOfTwo(std::uint32_t value);
std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment);

}  // namespace agent_io
