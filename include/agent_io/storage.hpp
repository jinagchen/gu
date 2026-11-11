#pragma once

#include "engine.hpp"

#include <array>
#include <cstddef>
#include <functional>

namespace agent_io {

using FileId = std::uint64_t;
using RequestId = std::uint64_t;

enum class ReadPriority { demand, prefetch, background };

struct BlockKey {
    FileId file_id;
    std::uint64_t aligned_offset;
    std::uint32_t byte_count;
    bool operator==(const BlockKey&) const = default;
};

class BufferLease {
public:
    virtual ~BufferLease() = default;
    virtual std::span<std::byte> WritableBytes() noexcept = 0;
    virtual std::span<const std::byte> Bytes() const noexcept = 0;
};

// The implementation pins both the native handle and immutable file identity.
class FileLease {
public:
    virtual ~FileLease() = default;
    virtual FileId Id() const noexcept = 0;
    virtual std::uint64_t SizeBytes() const noexcept = 0;
};

struct ReadRequest {
    RequestId request_id;
    BlockKey block;
    ReadPriority priority;
    std::shared_ptr<const FileLease> file;
    std::shared_ptr<BufferLease> buffer;
};

struct ReadCompletion {
    RequestId request_id;
    std::uint32_t bytes_completed;
};

using ReadCallback = std::function<void(Result<ReadCompletion>)>;

// Accepted requests retain file/buffer leases until completion, including drain.
class ReadBackend {
public:
    virtual ~ReadBackend() = default;
    virtual Result<Success> Submit(
        ReadRequest request, ReadCallback completion) = 0;
    virtual Result<Success> Drain() = 0;
};

struct BlockLocation {
    BlockKey key;
    std::shared_ptr<const FileLease> file;
};

// Implementations pin immutable bytes, the file, and their budget charge.
class BlockLease {
public:
    virtual ~BlockLease() = default;
    virtual BlockKey Key() const noexcept = 0;
    virtual std::span<const std::byte> Bytes() const noexcept = 0;
};

using BlockLeasePtr = std::shared_ptr<const BlockLease>;

struct BlockReadOptions {
    ReadPriority priority = ReadPriority::demand;
    std::chrono::steady_clock::time_point deadline;
};

enum class PrefetchDisposition { admitted, already_present, dropped };

// Batch order matches input order; errors never expose partially filled blocks.
class BlockReader {
public:
    virtual ~BlockReader() = default;
    virtual Result<std::vector<BlockLeasePtr>> ReadBatch(
        std::span<const BlockLocation> blocks, const BlockReadOptions& options) = 0;
    virtual Result<PrefetchDisposition> TryPrefetch(
        const BlockLocation& block,
        std::chrono::steady_clock::time_point deadline) = 0;
    virtual Result<Success> Drain() = 0;
};

// Retire follows B's durable publication decision; existing leases stay readable.
class FileRegistry {
public:
    virtual ~FileRegistry() = default;
    virtual Result<std::shared_ptr<const FileLease>> Open(
        const std::filesystem::path& path) = 0;
    virtual Result<Success> Retire(FileId file) = 0;
    virtual Result<Success> CollectRetired() = 0;
};

enum class MemoryRegion { cache, delta, metadata, workspace };

struct MemoryCharge {
    MemoryRegion region;
    std::uint64_t bytes;
};

// The last owner releases the charge; all live allocations retain their token.
class BudgetReservation {
public:
    virtual ~BudgetReservation() = default;
    virtual MemoryCharge Charge() const noexcept = 0;
};

enum class BudgetMode { query, rebuild };

class ResourceGovernor {
public:
    virtual ~ResourceGovernor() = default;
    virtual Result<std::shared_ptr<const BudgetReservation>> TryReserve(
        MemoryCharge charge) = 0;
    virtual Result<Success> TrySwitchMode(BudgetMode mode) = 0;
    virtual BudgetMode Mode() const noexcept = 0;
};

enum class WalOperation : std::uint16_t { insert = 1, upsert = 2 };

struct WalFileHeader {
    std::array<std::byte, 16> instance_id;
    Sequence start_sequence;
};

struct WalRecord {
    WalOperation operation;
    Sequence sequence;
    VectorId id;
    std::span<const float> values;
};

enum class WalDecodeStatus { complete, need_more };

struct DecodedWalFrame {
    WalDecodeStatus status;
    WalOperation operation;
    Sequence sequence;
    VectorId id;
    std::uint32_t dimension;
    // Borrowed little-endian float32 bytes, valid only while input stays alive.
    std::span<const std::byte> payload;
    std::uint64_t consumed_bytes;
};

// Declaration-only FORMAT-1 codecs; callers supply already reserved storage.
Result<std::uint64_t> WalFrameBytes(std::uint32_t dimension);
Result<Success> EncodeWalFileHeader(
    const WalFileHeader& header, std::span<std::byte> output);
Result<WalFileHeader> DecodeWalFileHeader(std::span<const std::byte> input);
Result<std::uint64_t> EncodeWalFrame(
    const WalRecord& record, std::span<std::byte> output);
Result<DecodedWalFrame> DecodeWalFrame(
    std::span<const std::byte> input, std::uint32_t expected_dimension,
    std::uint32_t max_frame_bytes);

class WalWriter {
public:
    virtual ~WalWriter() = default;
    virtual Result<Success> Append(std::span<const std::byte> encoded_frame) = 0;
    virtual Result<Success> Sync() = 0;
    virtual Result<Success> Rotate(const WalFileHeader& next) = 0;
    virtual Result<Success> Close() = 0;
};

Result<std::unique_ptr<WalWriter>> CreateWalWriter(
    const std::filesystem::path& directory, const WalFileHeader& header,
    std::uint32_t dimension, const WalOptions& options);

}  // namespace agent_io
