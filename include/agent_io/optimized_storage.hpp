#pragma once

#include "synchronous_storage.hpp"

#include <atomic>

namespace agent_io {

Result<std::unique_ptr<MeteredReadBackend>> CreateIocpReadBackend(
    std::shared_ptr<ResourceGovernor> governor, std::uint32_t max_inflight_reads);

struct OptimizedReaderOptions {
    std::uint32_t max_inflight_reads;
    std::uint32_t max_prefetch_reads;
    bool cache_enabled;
    // Opt-in until the Engine has a maintenance caller.
    std::optional<ReadScheduling> scheduling = std::nullopt;
};

struct BlockReaderStats {
    std::uint64_t cache_hits = 0;
    std::uint64_t cache_misses = 0;
    std::uint64_t cache_resident_bytes = 0;
    std::uint64_t prefetch_submitted = 0;
    std::uint64_t prefetch_consumed = 0;
    std::uint64_t prefetch_ready_at_demand = 0;
    std::uint32_t peak_inflight_reads = 0;
    std::uint32_t peak_prefetch_reads = 0;
    std::uint64_t background_submitted = 0;
    std::size_t queued_background_reads = 0;
    std::uint32_t peak_background_reads = 0;
};

// The caller and admitted I/O share this state, including query slot ownership.
struct QueryReadState {
    std::shared_ptr<const void> admission;
    std::shared_ptr<const BudgetReservation> workspace;
    std::uint32_t prefetch_limit = 0;
    std::atomic<std::uint32_t> outstanding_prefetch{0};
    std::uint64_t cache_hits = 0;
    std::uint64_t cache_misses = 0;
};

// The static adapter executes reads on the search caller's thread.
class QueryReadScope {
    std::shared_ptr<QueryReadState> previous_;
public:
    explicit QueryReadScope(std::shared_ptr<QueryReadState> state);
    ~QueryReadScope();
    QueryReadScope(const QueryReadScope&) = delete;
    QueryReadScope& operator=(const QueryReadScope&) = delete;
};

class OptimizedBlockReader : public BlockReader {
public:
    virtual BlockReaderStats Stats() const = 0;
    virtual void Retire(FileId file) = 0;
    virtual Result<Success> Quiesce(std::chrono::steady_clock::time_point deadline) = 0;
};

Result<std::unique_ptr<OptimizedBlockReader>> CreateOptimizedBlockReader(
    std::shared_ptr<ResourceGovernor> governor,
    std::shared_ptr<MeteredReadBackend> backend, const OptimizedReaderOptions& options);

}  // namespace agent_io
