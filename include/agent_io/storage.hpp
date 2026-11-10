#pragma once

#include "engine.hpp"

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

class WalWriter {
public:
    virtual ~WalWriter() = default;
    virtual Result<Success> Append(std::span<const std::byte> encoded_frame) = 0;
    virtual Result<Success> Sync() = 0;
};

}  // namespace agent_io
