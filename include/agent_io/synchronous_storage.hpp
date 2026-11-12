#pragma once

#include "storage.hpp"

namespace agent_io {

// Factories add implementations without changing the API-1.2 interfaces.
Result<std::shared_ptr<ResourceGovernor>> CreateResourceGovernor(
    const MemoryBudget& query,
    const std::optional<MemoryBudget>& rebuild = std::nullopt);

struct SynchronousStorageOptions {
    bool direct_reads = true;
    std::uint32_t max_registered_files = 64;
    std::uint32_t max_inflight_reads = 64;
};

Result<std::unique_ptr<FileRegistry>> CreateSynchronousFileRegistry(
    std::shared_ptr<ResourceGovernor> governor,
    const SynchronousStorageOptions& options = {});

// The buffer owns its reservation until after the allocation has been freed.
Result<std::shared_ptr<BufferLease>> AllocateReadBuffer(
    std::shared_ptr<ResourceGovernor> governor, std::uint32_t bytes,
    std::uint32_t alignment, MemoryRegion region = MemoryRegion::workspace);

// Required address, offset and length alignment for a file from this registry.
Result<std::uint32_t> ReadAlignment(const FileLease& file);

struct ReadBackendStats {
    std::uint64_t io_reads_submitted = 0;
    std::uint64_t io_read_bytes_completed = 0;
    std::uint64_t io_read_errors = 0;
    std::uint32_t inflight_reads = 0;
};

// Successful Submit completes its callback before returning.
class SynchronousReadBackend : public ReadBackend {
public:
    virtual ReadBackendStats Stats() const = 0;
};

Result<std::unique_ptr<SynchronousReadBackend>> CreateSynchronousReadBackend(
    std::shared_ptr<ResourceGovernor> governor,
    const SynchronousStorageOptions& options = {});

// Static demand-read baseline: no cache, prefetch or maintenance scheduling.
Result<std::unique_ptr<BlockReader>> CreateSynchronousBlockReader(
    std::shared_ptr<ResourceGovernor> governor,
    std::shared_ptr<SynchronousReadBackend> backend,
    std::uint32_t max_batch_blocks = 64);

}  // namespace agent_io
