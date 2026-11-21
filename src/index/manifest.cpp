#include "format.hpp"

#include "digest.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace agent_io {
namespace {

using index_json::Value;

struct DecodeFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

Error Invalid(std::string message) {
    return Error{ErrorCode::invalid_argument, std::move(message)};
}

Error Corrupt(std::string message) {
    return Error{ErrorCode::corruption, std::move(message)};
}

bool AllZero(const InstanceId& id) {
    return std::all_of(id.begin(), id.end(), [](std::byte item) { return item == std::byte{0}; });
}

bool IsLowerHex(std::string_view text, std::size_t length) {
    if (text.size() != length) return false;
    return std::all_of(text.begin(), text.end(), [](char item) {
        return (item >= '0' && item <= '9') || (item >= 'a' && item <= 'f');
    });
}

const char* KindText(SegmentKind kind) {
    switch (kind) {
    case SegmentKind::base: return "base";
    case SegmentKind::merged: return "merged";
    case SegmentKind::flushed_delta: return "flushed_delta";
    }
    return "";
}

bool ParseKind(std::string_view text, SegmentKind& kind) {
    if (text == "base") { kind = SegmentKind::base; return true; }
    if (text == "merged") { kind = SegmentKind::merged; return true; }
    if (text == "flushed_delta") { kind = SegmentKind::flushed_delta; return true; }
    return false;
}

const Value& Require(const Value& object, std::string_view key) {
    const Value* member = object.Find(key);
    if (member == nullptr) throw DecodeFailure("Missing field: " + std::string(key));
    return *member;
}

std::string_view Text(const Value& value, std::string_view name, std::size_t maximum,
                      bool allow_empty = false) {
    if (!value.IsString()) throw DecodeFailure("Field must be a string: " + std::string(name));
    const auto& text = value.AsString();
    if ((!allow_empty && text.empty()) || text.size() > maximum)
        throw DecodeFailure("String length out of range: " + std::string(name));
    if (!index_json::IsValidUtf8(text))
        throw DecodeFailure("String is not valid UTF-8: " + std::string(name));
    for (const char item : text) {
        const auto raw = static_cast<unsigned char>(item);
        if (raw < 0x20 || raw == 0x7f)
            throw DecodeFailure("String contains a control character: " + std::string(name));
    }
    return text;
}

std::uint64_t Decimal(const Value& value, std::string_view name) {
    const auto text = Text(value, name, 20);
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

std::uint32_t Integer(const Value& value, std::string_view name, bool positive,
                      std::uint32_t maximum = 0xffffffffu) {
    if (!value.IsInteger() || value.Negative())
        throw DecodeFailure("Field must be a non-negative integer: " + std::string(name));
    const std::uint64_t number = value.Magnitude();
    if (number > maximum || (positive && number == 0))
        throw DecodeFailure("Integer out of range: " + std::string(name));
    return static_cast<std::uint32_t>(number);
}

Digest Sha(const Value& value, std::string_view name) {
    const auto text = Text(value, name, 64);
    const auto parsed = ParseDigest(text);
    if (const auto* error = std::get_if<Error>(&parsed))
        throw DecodeFailure(error->message + ": " + std::string(name));
    return std::get<Digest>(parsed);
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

std::string TextOf(const Value& value, std::string_view name, std::size_t maximum) {
    const auto text = Text(value, name, maximum);
    return std::string(text);
}

void PutString(Value& object, std::string_view key, std::string_view text) {
    object.Insert(std::string(key), Value::String(std::string(text)));
}

void PutU64(Value& object, std::string_view key, std::uint64_t value) {
    object.Insert(std::string(key), Value::String(std::to_string(value)));
}

void PutU32(Value& object, std::string_view key, std::uint32_t value) {
    object.Insert(std::string(key), Value::Unsigned(value));
}

// Shared validation: the same rules gate encoding and decoding, so a manifest
// that Decode accepts is exactly one Encode would emit.
Result<Success> Validate(const Manifest& manifest, bool for_encoding) {
    if (manifest.format_version != 1) {
        return for_encoding ? Invalid("Unsupported manifest format version") :
                              Error{ErrorCode::unsupported, "Unsupported manifest format version"};
    }
    if (AllZero(manifest.instance_id)) return Invalid("Manifest instance id must be non-zero");
    if (manifest.generation == 0) return Invalid("Manifest generation must be positive");
    if (manifest.dimension == 0 || manifest.dimension > 65536)
        return Invalid("Manifest dimension is out of range");
    if (manifest.metric != Metric::squared_l2) return Invalid("Unsupported metric");
    if (!IsPowerOfTwo(manifest.block_bytes)) return Invalid("Block size must be a power of two");
    const auto layout_status = ValidateLayout(manifest.layout, manifest.dimension,
                                              manifest.block_bytes);
    if (const auto* error = std::get_if<Error>(&layout_status)) return *error;
    if (manifest.segment_count > kMaxManifestSegments)
        return Invalid("Too many manifest segments");
    if (manifest.wal_replay_after != manifest.checkpoint_sequence)
        return Invalid("wal_replay_after must equal checkpoint_sequence");
    if (manifest.adapter.name.size == 0 || manifest.adapter.commit.size == 0 ||
        manifest.adapter.native_version.size == 0 || manifest.adapter.parameters.size == 0)
        return Invalid("Adapter fields must be non-empty");
    if (!IsLowerHex(ManifestTextView(manifest.adapter.commit), 40))
        return Invalid("Adapter commit must be 40 lowercase hexadecimal characters");
    if (manifest.segment_count == 0) {
        if (manifest.checkpoint_sequence != 0 || manifest.live_count != 0)
            return Invalid("An index without segments must have checkpoint and live_count zero");
        return Success{};
    }
    std::set<SegmentId> ids;
    std::set<std::string> uuids;
    std::uint64_t total_nodes = 0;
    for (std::uint32_t index = 0; index < manifest.segment_count; ++index) {
        const auto& entry = manifest.segments[index];
        if (entry.id == 0) return Invalid("Segment id must be non-zero");
        if (!ids.insert(entry.id).second) return Invalid("Duplicate segment id");
        if (AllZero(entry.uuid)) return Invalid("Segment uuid must be non-zero");
        if (!uuids.insert(FormatUuid(entry.uuid)).second) return Invalid("Duplicate segment uuid");
        if (entry.generation == 0 || entry.generation > manifest.generation)
            return Invalid("Segment generation is out of range");
        if (entry.covered_sequence > manifest.checkpoint_sequence)
            return Invalid("Segment covers a sequence beyond the checkpoint");
        if (entry.kind == SegmentKind::base && entry.covered_sequence != 0)
            return Invalid("A base segment must cover sequence zero");
        if (entry.node_count == 0 || entry.node_count > 0xffffffffull)
            return Invalid("Segment node count is out of range");
        if (entry.file_bytes == 0 || entry.file_bytes % manifest.block_bytes != 0)
            return Invalid("Segment file bytes must be a block multiple");
        const auto minimum = entry.node_count * manifest.layout.row_bytes;
        if (entry.file_bytes < minimum) return Invalid("Segment file is too small for its rows");
        if (entry.metadata.logical_bytes == 0 ||
            entry.metadata.physical_bytes < entry.metadata.logical_bytes)
            return Invalid("Segment metadata lengths are inconsistent");
        const std::string expected = "segments/" + FormatUuid(entry.uuid) + "/segment.json";
        if (ManifestTextView(entry.metadata.path) != expected)
            return Invalid("Segment metadata path is not canonical");
        if (total_nodes > std::numeric_limits<std::uint64_t>::max() - entry.node_count)
            return Invalid("Segment node counts overflow");
        total_nodes += entry.node_count;
    }
    if (manifest.live_count > total_nodes)
        return Invalid("live_count exceeds the number of stored vectors");
    return Success{};
}

Result<std::string> BuildJson(const Manifest& manifest) {
    const auto status = Validate(manifest, true);
    if (const auto* error = std::get_if<Error>(&status)) return *error;

    Value object = Value::Object();
    PutU32(object, "format_version", manifest.format_version);
    PutString(object, "instance_id", FormatUuid(manifest.instance_id));
    PutU64(object, "generation", manifest.generation);
    PutU64(object, "checkpoint_sequence", manifest.checkpoint_sequence);
    PutU32(object, "dimension", manifest.dimension);
    PutString(object, "metric", "squared_l2");
    PutU32(object, "block_bytes", manifest.block_bytes);
    PutU64(object, "live_count", manifest.live_count);
    Value adapter = Value::Object();
    PutString(adapter, "name", ManifestTextView(manifest.adapter.name));
    PutString(adapter, "commit", ManifestTextView(manifest.adapter.commit));
    PutString(adapter, "native_version", ManifestTextView(manifest.adapter.native_version));
    PutString(adapter, "parameters", ManifestTextView(manifest.adapter.parameters));
    object.Insert("adapter", std::move(adapter));
    Value layout = Value::Object();
    PutU32(layout, "dimension", manifest.layout.dimension);
    PutU32(layout, "max_degree", manifest.layout.max_degree);
    PutU32(layout, "row_bytes", manifest.layout.row_bytes);
    PutU32(layout, "vector_offset", manifest.layout.vector_offset);
    PutU32(layout, "neighbor_offset", manifest.layout.neighbor_offset);
    PutU32(layout, "metadata_offset", manifest.layout.metadata_offset);
    object.Insert("layout", std::move(layout));
    Value segments = Value::Array();
    for (std::uint32_t index = 0; index < manifest.segment_count; ++index) {
        const auto& entry = manifest.segments[index];
        Value item = Value::Object();
        PutU64(item, "id", entry.id);
        PutString(item, "uuid", FormatUuid(entry.uuid));
        PutU64(item, "generation", entry.generation);
        PutString(item, "kind", KindText(entry.kind));
        PutU64(item, "covered_sequence", entry.covered_sequence);
        PutU64(item, "node_count", entry.node_count);
        PutU64(item, "file_bytes", entry.file_bytes);
        PutString(item, "content_sha256", FormatDigest(entry.content_sha256));
        Value metadata = Value::Object();
        PutString(metadata, "path", ManifestTextView(entry.metadata.path));
        PutU64(metadata, "logical_bytes", entry.metadata.logical_bytes);
        PutU64(metadata, "physical_bytes", entry.metadata.physical_bytes);
        PutString(metadata, "sha256", FormatDigest(entry.metadata.sha256));
        item.Insert("metadata", std::move(metadata));
        segments.Push(std::move(item));
    }
    object.Insert("segments", std::move(segments));
    PutU64(object, "wal_replay_after", manifest.wal_replay_after);
    auto text = index_json::Write(object);
    if (text.empty()) return Invalid("Manifest encoding failed");
    if (text.size() > kMaxManifestBytes) return Invalid("Manifest exceeds its size bound");
    return text;
}

}  // namespace

Result<std::uint64_t> ManifestBytes(const Manifest& manifest) {
    const auto text = BuildJson(manifest);
    if (const auto* error = std::get_if<Error>(&text)) return *error;
    return static_cast<std::uint64_t>(std::get<std::string>(text).size());
}

Result<std::uint64_t> EncodeManifest(const Manifest& manifest, std::span<std::byte> output) {
    const auto text = BuildJson(manifest);
    if (const auto* error = std::get_if<Error>(&text)) return *error;
    const auto& encoded = std::get<std::string>(text);
    if (output.size() < encoded.size())
        return Invalid("Manifest output buffer is too small");
    for (std::size_t index = 0; index < encoded.size(); ++index)
        output[index] = static_cast<std::byte>(static_cast<unsigned char>(encoded[index]));
    return static_cast<std::uint64_t>(encoded.size());
}

Result<Manifest> DecodeManifest(std::span<const std::byte> input) {
    try {
        if (input.size() > kMaxManifestBytes)
            return Corrupt("Manifest exceeds its size bound");
        const std::string_view text(reinterpret_cast<const char*>(input.data()), input.size());
        const auto parsed = index_json::Parse(text);
        if (!parsed.ok) throw DecodeFailure(parsed.error);
        Fields fields(parsed.value);
        Manifest manifest;
        manifest.format_version = Integer(fields.Take("format_version"), "format_version", true);
        if (manifest.format_version != 1)
            return Error{ErrorCode::unsupported, "Unsupported manifest format version"};
        {
            const auto uuid = Text(fields.Take("instance_id"), "instance_id", 36);
            const auto parsed_uuid = ParseUuid(uuid);
            if (const auto* error = std::get_if<Error>(&parsed_uuid)) return *error;
            manifest.instance_id = std::get<InstanceId>(parsed_uuid);
        }
        manifest.generation = Decimal(fields.Take("generation"), "generation");
        manifest.checkpoint_sequence = Decimal(fields.Take("checkpoint_sequence"),
                                               "checkpoint_sequence");
        manifest.dimension = Integer(fields.Take("dimension"), "dimension", true, 65536);
        if (Text(fields.Take("metric"), "metric", 32) != "squared_l2")
            return Error{ErrorCode::unsupported, "Unsupported metric"};
        manifest.block_bytes = Integer(fields.Take("block_bytes"), "block_bytes", true);
        manifest.metric = Metric::squared_l2;
        manifest.live_count = Decimal(fields.Take("live_count"), "live_count");
        {
            Fields adapter(fields.Take("adapter"));
            const auto name = Text(adapter.Take("name"), "adapter.name", kManifestNameBytes);
            const auto commit = Text(adapter.Take("commit"), "adapter.commit", kManifestNameBytes);
            const auto version = Text(adapter.Take("native_version"), "adapter.native_version",
                                      kManifestNameBytes);
            const auto parameters = Text(adapter.Take("parameters"), "adapter.parameters",
                                         kManifestParameterBytes);
            adapter.Finish();
            if (!IsLowerHex(commit, 40))
                return Corrupt("Adapter commit must be 40 lowercase hexadecimal characters");
            if (auto status = SetManifestText(manifest.adapter.name, name); !std::holds_alternative<Success>(status))
                return std::get<Error>(status);
            if (auto status = SetManifestText(manifest.adapter.commit, commit); !std::holds_alternative<Success>(status))
                return std::get<Error>(status);
            if (auto status = SetManifestText(manifest.adapter.native_version, version); !std::holds_alternative<Success>(status))
                return std::get<Error>(status);
            if (auto status = SetManifestText(manifest.adapter.parameters, parameters); !std::holds_alternative<Success>(status))
                return std::get<Error>(status);
        }
        {
            Fields layout(fields.Take("layout"));
            manifest.layout.dimension = Integer(layout.Take("dimension"), "layout.dimension", true);
            manifest.layout.max_degree = Integer(layout.Take("max_degree"), "layout.max_degree", true);
            manifest.layout.row_bytes = Integer(layout.Take("row_bytes"), "layout.row_bytes", true);
            manifest.layout.vector_offset = Integer(layout.Take("vector_offset"),
                                                    "layout.vector_offset", false);
            manifest.layout.neighbor_offset = Integer(layout.Take("neighbor_offset"),
                                                      "layout.neighbor_offset", false);
            manifest.layout.metadata_offset = Integer(layout.Take("metadata_offset"),
                                                      "layout.metadata_offset", false);
            layout.Finish();
        }
        const Value& segments = fields.Take("segments");
        if (!segments.IsArray() || segments.Size() > kMaxManifestSegments)
            return Corrupt("Manifest segment array is out of range");
        manifest.segment_count = static_cast<std::uint32_t>(segments.Size());
        for (std::uint32_t index = 0; index < manifest.segment_count; ++index) {
            Fields item(segments.At(index));
            auto& entry = manifest.segments[index];
            entry.id = Decimal(item.Take("id"), "segments.id");
            {
                const auto uuid = Text(item.Take("uuid"), "segments.uuid", 36);
                const auto parsed_uuid = ParseUuid(uuid);
                if (const auto* error = std::get_if<Error>(&parsed_uuid)) return *error;
                entry.uuid = std::get<InstanceId>(parsed_uuid);
            }
            entry.generation = Decimal(item.Take("generation"), "segments.generation");
            const auto kind = Text(item.Take("kind"), "segments.kind", 32);
            if (!ParseKind(kind, entry.kind)) return Corrupt("Unknown segment kind");
            entry.covered_sequence = Decimal(item.Take("covered_sequence"),
                                             "segments.covered_sequence");
            entry.node_count = Decimal(item.Take("node_count"), "segments.node_count");
            entry.file_bytes = Decimal(item.Take("file_bytes"), "segments.file_bytes");
            entry.content_sha256 = Sha(item.Take("content_sha256"), "segments.content_sha256");
            Fields metadata(item.Take("metadata"));
            {
                const auto path = Text(metadata.Take("path"), "metadata.path", kManifestPathBytes);
                entry.metadata.logical_bytes = Decimal(metadata.Take("logical_bytes"),
                                                       "metadata.logical_bytes");
                entry.metadata.physical_bytes = Decimal(metadata.Take("physical_bytes"),
                                                        "metadata.physical_bytes");
                entry.metadata.sha256 = Sha(metadata.Take("sha256"), "metadata.sha256");
                metadata.Finish();
                if (auto status = SetManifestText(entry.metadata.path, path);
                    !std::holds_alternative<Success>(status))
                    return std::get<Error>(status);
            }
            item.Finish();
        }
        manifest.wal_replay_after = Decimal(fields.Take("wal_replay_after"), "wal_replay_after");
        fields.Finish();
        const auto status = Validate(manifest, false);
        if (const auto* error = std::get_if<Error>(&status)) return *error;
        return manifest;
    } catch (const DecodeFailure& failure) {
        return Corrupt(failure.what());
    }
}

}  // namespace agent_io
