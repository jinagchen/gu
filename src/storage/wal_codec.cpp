#include "agent_io/storage.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <string_view>

namespace agent_io {
namespace {

constexpr std::size_t kHeaderBytes = 64;
constexpr std::size_t kCrcBytes = 4;
constexpr std::size_t kFloatBytes = 4;
constexpr std::string_view kFileMagic = "AGIOLOG1";
constexpr std::string_view kFrameMagic = "AGIOWAL1";
static_assert(sizeof(float) == kFloatBytes && std::numeric_limits<float>::is_iec559);

using Bytes = std::span<const std::byte>;
using Output = std::span<std::byte>;

Error Invalid(const char* message) {
    return {ErrorCode::invalid_argument, message, 0, WriteEffect::none, {}};
}

Error Corrupt(const char* message) {
    return {ErrorCode::corruption, message, 0, WriteEffect::none, {}};
}

constexpr auto CrcTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < table.size(); ++i) {
        auto crc = i;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) != 0 ? 0x82f63b78u : 0u);
        table[i] = crc;
    }
    return table;
}

constexpr auto kCrcTable = CrcTable();

constexpr std::uint32_t Crc32c(Bytes bytes) {
    std::uint32_t crc = 0xffffffffu;
    for (const auto byte : bytes)
        crc = (crc >> 8) ^ kCrcTable[(crc ^ std::to_integer<unsigned>(byte)) & 0xffu];
    return crc ^ 0xffffffffu;
}

static_assert(Crc32c(std::array{
    std::byte{'1'}, std::byte{'2'}, std::byte{'3'}, std::byte{'4'}, std::byte{'5'},
    std::byte{'6'}, std::byte{'7'}, std::byte{'8'}, std::byte{'9'}}) == 0xe3069283u);

constexpr std::uint64_t FrameSize(std::uint32_t dimension) noexcept {
    // The uint32 dimension cannot overflow this uint64 calculation.
    return (kHeaderBytes + static_cast<std::uint64_t>(dimension) * kFloatBytes +
            kCrcBytes + 7u) & ~std::uint64_t{7};
}

void Put(Output bytes, std::size_t offset, std::size_t width, std::uint64_t value) {
    for (std::size_t i = 0; i < width; ++i)
        bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xffu);
}

std::uint64_t Get(Bytes bytes, std::size_t offset, std::size_t width) {
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < width; ++i)
        result |= std::to_integer<std::uint64_t>(bytes[offset + i]) << (8 * i);
    return result;
}

// Even a partial fixed field can prove the header invalid.
bool Matches(Bytes bytes, std::size_t offset, std::size_t width, std::uint64_t value) {
    for (std::size_t i = 0; i < width && offset + i < bytes.size(); ++i)
        if (bytes[offset + i] != static_cast<std::byte>((value >> (8 * i)) & 0xffu))
            return false;
    return true;
}

bool MatchesMagic(Bytes bytes, std::string_view magic) {
    for (std::size_t i = 0; i < magic.size() && i < bytes.size(); ++i)
        if (bytes[i] != static_cast<std::byte>(magic[i])) return false;
    return true;
}

void PutMagic(Output bytes, std::string_view magic) {
    for (std::size_t i = 0; i < magic.size(); ++i)
        bytes[i] = static_cast<std::byte>(magic[i]);
}

bool AllZero(Bytes bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](std::byte b) { return b == std::byte{}; });
}

bool IsFiniteBits(std::uint32_t bits) { return (bits & 0x7f800000u) != 0x7f800000u; }

}  // namespace

Result<std::uint64_t> WalFrameBytes(std::uint32_t dimension) {
    if (dimension == 0) return Invalid("WAL dimension must be positive");
    return FrameSize(dimension);
}

Result<Success> EncodeWalFileHeader(const WalFileHeader& header, Output output) {
    if (output.size() != kHeaderBytes || header.start_sequence == 0 || AllZero(header.instance_id))
        return Invalid("Invalid WAL file header or buffer size");
    std::fill(output.begin(), output.end(), std::byte{});
    PutMagic(output, kFileMagic);
    Put(output, 8, 4, 1);
    Put(output, 12, 4, kHeaderBytes);
    std::copy(header.instance_id.begin(), header.instance_id.end(), output.begin() + 16);
    Put(output, 32, 8, header.start_sequence);
    Put(output, 60, 4, Crc32c(output.first(60)));
    return Success{};
}

Result<WalFileHeader> DecodeWalFileHeader(Bytes input) {
    if (input.size() != kHeaderBytes || !MatchesMagic(input, kFileMagic) ||
        Get(input, 8, 4) != 1 || Get(input, 12, 4) != kHeaderBytes ||
        AllZero(input.subspan(16, 16)) || Get(input, 32, 8) == 0 ||
        !AllZero(input.subspan(40, 20)) || Get(input, 60, 4) != Crc32c(input.first(60)))
        return Corrupt("Invalid FORMAT-1 WAL file header");
    WalFileHeader header{};
    std::copy_n(input.begin() + 16, header.instance_id.size(), header.instance_id.begin());
    header.start_sequence = Get(input, 32, 8);
    return header;
}

Result<std::uint64_t> EncodeWalFrame(const WalRecord& record, Output output) {
    if ((record.operation != WalOperation::insert && record.operation != WalOperation::upsert) ||
        record.sequence == 0 || record.values.empty() || record.values.size() > UINT32_MAX)
        return Invalid("Invalid WAL operation, sequence, or dimension");
    const auto dimension = static_cast<std::uint32_t>(record.values.size());
    const auto size = FrameSize(dimension);
    if (size > output.size())
        return Error{ErrorCode::resource_exhausted, "WAL frame buffer is too small",
                     0, WriteEffect::none, {}};
    for (const auto value : record.values)
        if (!IsFiniteBits(std::bit_cast<std::uint32_t>(value)))
            return Invalid("WAL payload must contain finite float32 values");

    const auto frame = output.first(static_cast<std::size_t>(size));
    std::fill(frame.begin(), frame.end(), std::byte{});
    PutMagic(frame, kFrameMagic);
    Put(frame, 8, 2, 1);
    Put(frame, 10, 2, static_cast<std::uint16_t>(record.operation));
    Put(frame, 12, 4, kHeaderBytes);
    Put(frame, 16, 8, size);
    Put(frame, 24, 8, record.sequence);
    Put(frame, 32, 8, record.id);
    Put(frame, 40, 4, dimension);
    Put(frame, 48, 8, static_cast<std::uint64_t>(dimension) * kFloatBytes);
    std::size_t cursor = kHeaderBytes;
    for (const auto value : record.values) {
        Put(frame, cursor, kFloatBytes, std::bit_cast<std::uint32_t>(value));
        cursor += kFloatBytes;
    }
    Put(frame, cursor, kCrcBytes, Crc32c(frame.first(cursor)));
    return size;
}

Result<DecodedWalFrame> DecodeWalFrame(Bytes input, std::uint32_t expected_dimension,
                                       std::uint32_t max_frame_bytes) {
    const auto size = FrameSize(expected_dimension);
    if (expected_dimension == 0 || size > max_frame_bytes)
        return Invalid("Invalid WAL decoder dimension or frame limit");
    const auto payload_size = static_cast<std::uint64_t>(expected_dimension) * kFloatBytes;
    if (!MatchesMagic(input, kFrameMagic) || !Matches(input, 8, 2, 1) ||
        (!Matches(input, 10, 2, 1) && !Matches(input, 10, 2, 2)) ||
        !Matches(input, 12, 4, kHeaderBytes) || !Matches(input, 16, 8, size) ||
        (input.size() >= 32 && Get(input, 24, 8) == 0) ||
        !Matches(input, 40, 4, expected_dimension) || !Matches(input, 44, 4, 0) ||
        !Matches(input, 48, 8, payload_size) || !Matches(input, 56, 8, 0))
        return Corrupt("Invalid FORMAT-1 WAL frame header");
    const auto frame = input.first((std::min)(input.size(), static_cast<std::size_t>(size)));
    const auto crc_offset = kHeaderBytes + static_cast<std::size_t>(payload_size);
    if ((frame.size() > crc_offset &&
         !Matches(frame, crc_offset, kCrcBytes, Crc32c(frame.first(crc_offset)))) ||
        (frame.size() > crc_offset + kCrcBytes &&
         !AllZero(frame.subspan(crc_offset + kCrcBytes))))
        return Corrupt("WAL frame checksum or padding is invalid");
    const auto payload_end = (std::min)(frame.size(), crc_offset);
    for (std::size_t i = kHeaderBytes; i + kFloatBytes <= payload_end; i += kFloatBytes)
        if (!IsFiniteBits(static_cast<std::uint32_t>(Get(frame, i, kFloatBytes))))
            return Corrupt("WAL frame payload is not finite float32");
    if (frame.size() < size)
        return DecodedWalFrame{WalDecodeStatus::need_more, {}, 0, 0, 0, {}, 0};

    const auto payload = frame.subspan(kHeaderBytes, static_cast<std::size_t>(payload_size));

    return DecodedWalFrame{WalDecodeStatus::complete, static_cast<WalOperation>(Get(frame, 10, 2)),
                           Get(frame, 24, 8), Get(frame, 32, 8), expected_dimension, payload, size};
}

}  // namespace agent_io
