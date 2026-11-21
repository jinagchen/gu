#include "format.hpp"

#include "digest.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace agent_io {
namespace {

using index_json::Value;

struct DecodeFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

const Value& Require(const Value& object, std::string_view key) {
    const Value* member = object.Find(key);
    if (member == nullptr) throw DecodeFailure("Missing field: " + std::string(key));
    return *member;
}

std::string_view AsString(const Value& value, std::string_view name, std::size_t maximum) {
    if (!value.IsString()) throw DecodeFailure("Field must be a string: " + std::string(name));
    const auto& text = value.AsString();
    if (text.empty() || text.size() > maximum)
        throw DecodeFailure("String length out of range: " + std::string(name));
    if (!index_json::IsValidUtf8(text))
        throw DecodeFailure("String is not valid UTF-8: " + std::string(name));
    return text;
}

std::uint64_t AsCanonicalU64(const Value& value, std::string_view name) {
    const auto text = AsString(value, name, 20);
    if (text.size() > 1 && text.front() == '0')
        throw DecodeFailure("Non-canonical decimal string: " + std::string(name));
    std::uint64_t result = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9')
            throw DecodeFailure("Non-numeric decimal string: " + std::string(name));
        const auto part = static_cast<std::uint64_t>(digit - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - part) / 10)
            throw DecodeFailure("Decimal string out of range: " + std::string(name));
        result = result * 10 + part;
    }
    return result;
}

std::uint32_t AsU32(const Value& value, std::string_view name, bool positive) {
    if (!value.IsInteger() || value.Negative())
        throw DecodeFailure("Field must be a non-negative integer: " + std::string(name));
    const std::uint64_t number = value.Magnitude();
    if (number > 0xffffffffull || (positive && number == 0))
        throw DecodeFailure("Integer out of range: " + std::string(name));
    return static_cast<std::uint32_t>(number);
}

Digest AsDigest(const Value& value, std::string_view name) {
    const auto text = AsString(value, name, 64);
    const auto parsed = ParseDigest(text);
    if (const auto* error = std::get_if<Error>(&parsed))
        throw DecodeFailure(error->message + ": " + std::string(name));
    return std::get<Digest>(parsed);
}

InstanceId AsUuid(const Value& value, std::string_view name) {
    const auto text = AsString(value, name, 36);
    const auto parsed = ParseUuid(text);
    if (const auto* error = std::get_if<Error>(&parsed))
        throw DecodeFailure(error->message + ": " + std::string(name));
    return std::get<InstanceId>(parsed);
}

class Fields {
public:
    explicit Fields(const Value& object) : object_(object) {
        if (!object.IsObject()) throw DecodeFailure("Expected a JSON object");
    }

    const Value& Take(std::string_view key) {
        taken_.emplace_back(key);
        return Require(object_, key);
    }

    void Finish() const {
        for (std::size_t index = 0; index < object_.MemberCount(); ++index) {
            const auto& name = object_.Member(index).first;
            if (std::find(taken_.begin(), taken_.end(), name) == taken_.end())
                throw DecodeFailure("Unknown field: " + name);
        }
        std::unordered_set<std::string> unique(taken_.begin(), taken_.end());
        if (unique.size() != taken_.size()) throw DecodeFailure("Duplicate field");
    }

private:
    const Value& object_;
    std::vector<std::string> taken_;
};

Value MakeObject() { return Value::Object(); }

void PutString(Value& object, std::string_view key, std::string_view text) {
    object.Insert(std::string(key), Value::String(std::string(text)));
}

void PutU64Text(Value& object, std::string_view key, std::uint64_t value) {
    object.Insert(std::string(key), Value::String(std::to_string(value)));
}

void PutU32(Value& object, std::string_view key, std::uint32_t value) {
    object.Insert(std::string(key), Value::Unsigned(value));
}

}  // namespace

bool IsPowerOfTwo(std::uint32_t value) { return value && (value & (value - 1)) == 0; }

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) {
    if (alignment == 0) return value;
    const auto remainder = value % alignment;
    return remainder == 0 ? value : value + (alignment - remainder);
}

Result<Success> ValidateLayout(const NodeRecordLayout& layout, std::uint32_t dimension,
                               std::uint32_t block_bytes) {
    if (dimension == 0 || dimension > 65536)
        return Error{ErrorCode::corruption, "Layout dimension is out of range"};
    if (layout.dimension != dimension)
        return Error{ErrorCode::corruption, "Layout dimension differs from the manifest"};
    if (layout.max_degree == 0 || layout.max_degree > 512)
        return Error{ErrorCode::corruption, "Layout max_degree is out of range"};
    if (!IsPowerOfTwo(block_bytes) || block_bytes > kMaxBlockBytes)
        return Error{ErrorCode::corruption, "Block size must be a bounded power of two"};
    if (layout.row_bytes == 0 || layout.row_bytes % block_bytes != 0)
        return Error{ErrorCode::corruption, "row_bytes must be a positive multiple of block_bytes"};
    const auto vector_bytes = static_cast<std::uint64_t>(dimension) * 4;
    const auto neighbor_bytes = 4 + static_cast<std::uint64_t>(layout.max_degree) * 4;
    const auto metadata_bytes = std::uint64_t{16};
    if (vector_bytes > layout.row_bytes) return Error{ErrorCode::corruption, "Vector exceeds row"};
    if (layout.vector_offset > layout.row_bytes - vector_bytes)
        return Error{ErrorCode::corruption, "Vector region exceeds the row"};
    if (neighbor_bytes > layout.row_bytes)
        return Error{ErrorCode::corruption, "Neighbour region exceeds row"};
    if (layout.neighbor_offset > layout.row_bytes - neighbor_bytes)
        return Error{ErrorCode::corruption, "Neighbour region exceeds the row"};
    if (metadata_bytes > layout.row_bytes ||
        layout.metadata_offset > layout.row_bytes - metadata_bytes)
        return Error{ErrorCode::corruption, "Metadata region exceeds the row"};
    const auto vector_end = layout.vector_offset + vector_bytes;
    const auto neighbor_end = layout.neighbor_offset + neighbor_bytes;
    const auto metadata_end = layout.metadata_offset + metadata_bytes;
    const bool overlap = (layout.vector_offset < neighbor_end && layout.neighbor_offset < vector_end) ||
        (layout.vector_offset < metadata_end && layout.metadata_offset < vector_end) ||
        (layout.neighbor_offset < metadata_end && layout.metadata_offset < neighbor_end);
    if (overlap) return Error{ErrorCode::corruption, "Layout regions overlap"};
    return Success{};
}

Result<std::string> EncodeCurrent(const CurrentRecord& current) {
    if (current.format_version != kCurrentFormatVersion)
        return Error{ErrorCode::invalid_argument, "Unsupported CURRENT version"};
    if (current.manifest.empty() || current.manifest.size() > kManifestPathBytes)
        return Error{ErrorCode::invalid_argument, "CURRENT manifest path is out of range"};
    if (!index_json::IsValidUtf8(current.manifest))
        return Error{ErrorCode::invalid_argument, "CURRENT path is not valid UTF-8"};
    if (current.manifest.find('\\') != std::string::npos ||
        current.manifest.find("..") != std::string::npos ||
        current.manifest.front() == '/' ||
        (current.manifest.size() > 1 && current.manifest[1] == ':'))
        return Error{ErrorCode::invalid_argument, "CURRENT path must be relative and inside the index"};
    Value object = MakeObject();
    PutU32(object, "format_version", current.format_version);
    PutString(object, "instance_id", FormatUuid(current.instance_id));
    PutString(object, "manifest", current.manifest);
    PutString(object, "sha256", FormatDigest(current.manifest_sha256));
    auto text = index_json::Write(object);
    if (text.empty() || text.size() > kMaxCurrentBytes)
        return Error{ErrorCode::invalid_argument, "CURRENT encoding failed"};
    return text;
}

Result<CurrentRecord> DecodeCurrent(std::string_view text) {
    try {
        if (text.size() > kMaxCurrentBytes) throw DecodeFailure("CURRENT exceeds its size bound");
        const auto parsed = index_json::Parse(text);
        if (!parsed.ok) throw DecodeFailure(parsed.error);
        Fields fields(parsed.value);
        CurrentRecord current;
        current.format_version = AsU32(fields.Take("format_version"), "format_version", true);
        current.instance_id = AsUuid(fields.Take("instance_id"), "instance_id");
        const auto manifest = AsString(fields.Take("manifest"), "manifest", kManifestPathBytes);
        current.manifest = std::string(manifest);
        current.manifest_sha256 = AsDigest(fields.Take("sha256"), "sha256");
        fields.Finish();
        if (current.format_version != kCurrentFormatVersion)
            return Error{ErrorCode::unsupported, "Unsupported CURRENT format version"};
        if (current.manifest.empty() || current.manifest.find("..") != std::string::npos ||
            current.manifest.find('\\') != std::string::npos ||
            current.manifest.front() == '/')
            return Error{ErrorCode::corruption, "CURRENT path escapes the index"};
        return current;
    } catch (const DecodeFailure& failure) {
        return Error{ErrorCode::corruption, failure.what()};
    }
}

Result<std::string> EncodeSegmentMetadata(const SegmentMetadata& metadata) {
    if (metadata.format_version != kSegmentFormatVersion)
        return Error{ErrorCode::invalid_argument, "Unsupported segment metadata version"};
    const auto layout_status = ValidateLayout(metadata.layout, metadata.dimension,
                                              metadata.block_bytes);
    if (const auto* error = std::get_if<Error>(&layout_status)) return *error;
    if (metadata.layout.row_bytes != metadata.row_bytes)
        return Error{ErrorCode::invalid_argument, "Row bytes disagree with the layout"};
    if (metadata.node_count == 0) return Error{ErrorCode::invalid_argument, "Empty segment"};
    if (metadata.files.empty()) return Error{ErrorCode::invalid_argument, "Segment lists no files"};
    if (metadata.version_directory_file.empty() ||
        metadata.version_entry_count != metadata.node_count)
        return Error{ErrorCode::invalid_argument, "Version directory does not match node count"};

    Value object = MakeObject();
    PutU32(object, "format_version", metadata.format_version);
    PutString(object, "segment_uuid", FormatUuid(metadata.uuid));
    PutU32(object, "dimension", metadata.dimension);
    PutString(object, "metric", "squared_l2");
    PutU32(object, "block_bytes", metadata.block_bytes);
    PutU32(object, "row_bytes", metadata.row_bytes);
    PutU64Text(object, "node_count", metadata.node_count);
    Value layout = MakeObject();
    PutU32(layout, "dimension", metadata.layout.dimension);
    PutU32(layout, "max_degree", metadata.layout.max_degree);
    PutU32(layout, "row_bytes", metadata.layout.row_bytes);
    PutU32(layout, "vector_offset", metadata.layout.vector_offset);
    PutU32(layout, "neighbor_offset", metadata.layout.neighbor_offset);
    PutU32(layout, "metadata_offset", metadata.layout.metadata_offset);
    object.Insert("layout", std::move(layout));
    Value builder = MakeObject();
    PutString(builder, "name", metadata.builder);
    PutString(builder, "version", metadata.builder_version);
    PutString(builder, "parameters", metadata.parameters);
    object.Insert("builder", std::move(builder));
    Value files = Value::Array();
    for (const auto& file : metadata.files) {
        Value entry = MakeObject();
        PutString(entry, "path", file.path);
        PutU64Text(entry, "logical_bytes", file.logical_bytes);
        PutU64Text(entry, "physical_bytes", file.physical_bytes);
        PutString(entry, "sha256", FormatDigest(file.sha256));
        files.Push(std::move(entry));
    }
    object.Insert("files", std::move(files));
    Value directory = MakeObject();
    PutString(directory, "file", metadata.version_directory_file);
    PutU64Text(directory, "entry_count", metadata.version_entry_count);
    PutU32(directory, "record_bytes", kVersionRecordBytes);
    object.Insert("version_directory", std::move(directory));
    auto text = index_json::Write(object);
    if (text.empty()) return Error{ErrorCode::invalid_argument, "Segment metadata encoding failed"};
    return text;
}

Result<SegmentMetadata> DecodeSegmentMetadata(std::string_view text) {
    try {
        const auto parsed = index_json::Parse(text);
        if (!parsed.ok) throw DecodeFailure(parsed.error);
        Fields fields(parsed.value);
        SegmentMetadata metadata;
        metadata.format_version = AsU32(fields.Take("format_version"), "format_version", true);
        metadata.uuid = AsUuid(fields.Take("segment_uuid"), "segment_uuid");
        metadata.dimension = AsU32(fields.Take("dimension"), "dimension", true);
        if (AsString(fields.Take("metric"), "metric", 32) != "squared_l2")
            return Error{ErrorCode::unsupported, "Unsupported metric"};
        metadata.block_bytes = AsU32(fields.Take("block_bytes"), "block_bytes", true);
        metadata.row_bytes = AsU32(fields.Take("row_bytes"), "row_bytes", true);
        metadata.node_count = AsCanonicalU64(fields.Take("node_count"), "node_count");
        const Value& layout = fields.Take("layout");
        {
            Fields layout_fields(layout);
            metadata.layout.dimension = AsU32(layout_fields.Take("dimension"), "layout.dimension", true);
            metadata.layout.max_degree = AsU32(layout_fields.Take("max_degree"), "layout.max_degree", true);
            metadata.layout.row_bytes = AsU32(layout_fields.Take("row_bytes"), "layout.row_bytes", true);
            metadata.layout.vector_offset = AsU32(layout_fields.Take("vector_offset"), "layout.vector_offset", false);
            metadata.layout.neighbor_offset = AsU32(layout_fields.Take("neighbor_offset"), "layout.neighbor_offset", false);
            metadata.layout.metadata_offset = AsU32(layout_fields.Take("metadata_offset"), "layout.metadata_offset", false);
            layout_fields.Finish();
        }
        const Value& builder = fields.Take("builder");
        {
            Fields builder_fields(builder);
            metadata.builder = std::string(AsString(builder_fields.Take("name"), "builder.name", 64));
            metadata.builder_version = std::string(
                AsString(builder_fields.Take("version"), "builder.version", 64));
            metadata.parameters = std::string(
                AsString(builder_fields.Take("parameters"), "builder.parameters", 512));
            builder_fields.Finish();
        }
        const Value& files = fields.Take("files");
        if (!files.IsArray() || files.Size() == 0 || files.Size() > 16)
            throw DecodeFailure("Segment file list is out of range");
        for (std::size_t index = 0; index < files.Size(); ++index) {
            Fields file_fields(files.At(index));
            SegmentFileRecord record;
            record.path = std::string(AsString(file_fields.Take("path"), "files.path", 128));
            record.logical_bytes = AsCanonicalU64(file_fields.Take("logical_bytes"),
                                                  "files.logical_bytes");
            record.physical_bytes = AsCanonicalU64(file_fields.Take("physical_bytes"),
                                                   "files.physical_bytes");
            record.sha256 = AsDigest(file_fields.Take("sha256"), "files.sha256");
            file_fields.Finish();
            if (record.logical_bytes == 0 || record.physical_bytes < record.logical_bytes)
                throw DecodeFailure("Segment file lengths are inconsistent");
            if (record.path.find("..") != std::string::npos ||
                record.path.find('\\') != std::string::npos || record.path.front() == '/')
                throw DecodeFailure("Segment file path escapes its directory");
            metadata.files.push_back(std::move(record));
        }
        const Value& directory = fields.Take("version_directory");
        {
            Fields directory_fields(directory);
            metadata.version_directory_file = std::string(AsString(
                directory_fields.Take("file"), "version_directory.file", 128));
            metadata.version_entry_count = AsCanonicalU64(
                directory_fields.Take("entry_count"), "version_directory.entry_count");
            if (AsU32(directory_fields.Take("record_bytes"), "record_bytes", true) !=
                kVersionRecordBytes)
                throw DecodeFailure("Unsupported version record size");
            directory_fields.Finish();
        }
        fields.Finish();
        if (metadata.format_version != kSegmentFormatVersion)
            return Error{ErrorCode::unsupported, "Unsupported segment metadata version"};
        if (metadata.version_entry_count != metadata.node_count)
            return Error{ErrorCode::corruption, "Version directory count differs from node count"};
        const auto layout_status = ValidateLayout(metadata.layout, metadata.dimension,
                                                  metadata.block_bytes);
        if (const auto* error = std::get_if<Error>(&layout_status)) return *error;
        if (metadata.layout.row_bytes != metadata.row_bytes)
            return Error{ErrorCode::corruption, "Row bytes disagree with the layout"};
        return metadata;
    } catch (const DecodeFailure& failure) {
        return Error{ErrorCode::corruption, failure.what()};
    }
}

Result<Success> EncodeVersionRecord(const VersionRecord& record, std::span<std::byte> output) {
    if (output.size() < kVersionRecordBytes)
        return Error{ErrorCode::invalid_argument, "Version record buffer is too small"};
    auto* bytes = output.data();
    const auto put64 = [&](std::size_t offset, std::uint64_t value) {
        for (std::size_t index = 0; index < 8; ++index)
            bytes[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xff);
    };
    put64(0, record.id);
    put64(8, record.version);
    put64(16, record.node);
    put64(24, 0);
    return Success{};
}

Result<VersionRecord> DecodeVersionRecord(std::span<const std::byte> input) {
    if (input.size() != kVersionRecordBytes)
        return Error{ErrorCode::invalid_argument, "Version record must be 32 bytes"};
    const auto get64 = [&](std::size_t offset) {
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < 8; ++index)
            value |= static_cast<std::uint64_t>(input[offset + index]) << (index * 8);
        return value;
    };
    VersionRecord record;
    record.id = get64(0);
    record.version = get64(8);
    const std::uint64_t node = get64(16);
    if (get64(24) != 0) return Error{ErrorCode::corruption, "Version record reserved bytes are set"};
    if (record.id == 0) return Error{ErrorCode::corruption, "Version record has a zero ID"};
    if (node > 0xffffffffull)
        return Error{ErrorCode::corruption, "Version record node id is out of range"};
    record.node = static_cast<NativeNodeId>(node);
    return record;
}

Result<NodeRow> DecodeNodeRow(std::span<const std::byte> row, const NodeRecordLayout& layout,
                              std::uint32_t dimension, std::vector<float>& vector_scratch) {
    if (row.size() < layout.row_bytes)
        return Error{ErrorCode::invalid_argument, "Node row is smaller than the layout"};
    if (layout.dimension != dimension)
        return Error{ErrorCode::corruption, "Row dimension differs from the index"};
    NodeRow result;
    result.neighbors.reserve(layout.max_degree);
    vector_scratch.resize(dimension);
    for (std::uint32_t index = 0; index < dimension; ++index) {
        std::uint32_t bits = 0;
        for (std::size_t byte = 0; byte < 4; ++byte)
            bits |= static_cast<std::uint32_t>(row[layout.vector_offset + index * 4 + byte])
                    << (byte * 8);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        vector_scratch[index] = value;
    }
    std::uint32_t count = 0;
    for (std::size_t byte = 0; byte < 4; ++byte)
        count |= static_cast<std::uint32_t>(row[layout.neighbor_offset + byte]) << (byte * 8);
    if (count > layout.max_degree)
        return Error{ErrorCode::corruption, "Neighbour count exceeds max_degree"};
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t node = 0;
        for (std::size_t byte = 0; byte < 4; ++byte)
            node |= static_cast<std::uint32_t>(
                        row[layout.neighbor_offset + 4 + index * 4 + byte]) << (byte * 8);
        result.neighbors.push_back(node);
    }
    std::uint64_t id = 0, version = 0;
    for (std::size_t byte = 0; byte < 8; ++byte) {
        id |= static_cast<std::uint64_t>(row[layout.metadata_offset + byte]) << (byte * 8);
        version |= static_cast<std::uint64_t>(row[layout.metadata_offset + 8 + byte])
                   << (byte * 8);
    }
    if (id == 0) return Error{ErrorCode::corruption, "Node row has a zero logical ID"};
    result.id = id;
    result.version = version;
    return result;
}

Result<Success> EncodeNodeRow(const NodeRow& row, std::span<const float> values,
                              const NodeRecordLayout& layout, std::span<std::byte> output) {
    if (output.size() < layout.row_bytes)
        return Error{ErrorCode::invalid_argument, "Node row buffer is smaller than the layout"};
    if (values.size() != layout.dimension)
        return Error{ErrorCode::invalid_argument, "Vector length differs from the layout"};
    if (row.neighbors.size() > layout.max_degree)
        return Error{ErrorCode::invalid_argument, "Neighbour count exceeds max_degree"};
    if (row.id == 0) return Error{ErrorCode::invalid_argument, "Logical ID must be non-zero"};
    std::fill(output.begin(), output.begin() + layout.row_bytes, std::byte{0});
    for (std::uint32_t index = 0; index < layout.dimension; ++index) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &values[index], sizeof(bits));
        for (std::size_t byte = 0; byte < 4; ++byte)
            output[layout.vector_offset + index * 4 + byte] =
                static_cast<std::byte>((bits >> (byte * 8)) & 0xff);
    }
    const auto count = static_cast<std::uint32_t>(row.neighbors.size());
    for (std::size_t byte = 0; byte < 4; ++byte)
        output[layout.neighbor_offset + byte] = static_cast<std::byte>((count >> (byte * 8)) & 0xff);
    for (std::uint32_t index = 0; index < count; ++index) {
        for (std::size_t byte = 0; byte < 4; ++byte)
            output[layout.neighbor_offset + 4 + index * 4 + byte] =
                static_cast<std::byte>((row.neighbors[index] >> (byte * 8)) & 0xff);
    }
    for (std::size_t byte = 0; byte < 8; ++byte) {
        output[layout.metadata_offset + byte] = static_cast<std::byte>((row.id >> (byte * 8)) & 0xff);
        output[layout.metadata_offset + 8 + byte] =
            static_cast<std::byte>((row.version >> (byte * 8)) & 0xff);
    }
    return Success{};
}

}  // namespace agent_io
