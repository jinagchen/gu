#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace agent_io {

using VectorId = std::uint64_t;
using Sequence = std::uint64_t;
using Generation = std::uint64_t;

enum class ErrorCode {
    invalid_argument,
    not_found,
    already_exists,
    resource_exhausted,
    deadline_exceeded,
    search_incomplete,
    io_error,
    corruption,
    unsupported,
    closed,
    recovery_required,
};

enum class WriteEffect { none, may_have_applied };

struct Error {
    ErrorCode code;
    std::string message;
    int system_error = 0;
    WriteEffect write_effect = WriteEffect::none;
    std::optional<Sequence> attempted_sequence;
};

template <class T>
using Result = std::variant<T, Error>;

struct Success {};

enum class Metric { squared_l2 };
enum class IoBackend { synchronous, windows_iocp };
enum class CachePolicy { disabled, clock };

struct MemoryBudget {
    std::uint64_t total_bytes = 0;
    std::uint64_t cache_bytes = 0;
    std::uint64_t delta_bytes = 0;
    std::uint64_t metadata_bytes = 0;
    std::uint64_t workspace_bytes = 0;
    std::uint64_t reserve_bytes = 0;
};

struct EngineOptions {
    MemoryBudget memory;
    IoBackend io_backend = IoBackend::synchronous;
    CachePolicy cache_policy = CachePolicy::clock;
    bool direct_reads = true;
    std::uint32_t block_bytes = 4096;
    std::uint32_t max_queries = 8;
    std::uint32_t max_pending_writes = 64;
    std::uint32_t max_inflight_reads = 64;
    std::uint32_t max_prefetch_reads = 16;
    std::uint32_t max_delta_segments = 4;
    std::chrono::milliseconds group_commit_window{1};
};

struct SearchOptions {
    std::uint32_t k = 10;
    std::uint32_t candidate_limit = 128;
    std::uint32_t prefetch_blocks = 0;
    std::chrono::milliseconds timeout{5000};
};

struct WriteOptions {
    std::chrono::milliseconds admission_timeout{5000};
};

struct Neighbor {
    VectorId id;
    Sequence version;
    double squared_distance;
};

struct QueryMetrics {
    std::uint64_t cache_hits = 0;
    std::uint64_t cache_misses = 0;
    std::uint64_t logical_block_demands = 0;
    std::uint64_t delta_records_scanned = 0;
    std::uint64_t elapsed_ns = 0;
    bool candidate_limit_reached = false;
};

struct SearchResult {
    Sequence snapshot_sequence;
    Generation view_generation;
    std::vector<Neighbor> neighbors;
    QueryMetrics metrics;
};

struct StoredVector {
    VectorId id;
    Sequence version;
    Sequence snapshot_sequence;
    std::vector<float> values;
};

struct WriteReceipt {
    VectorId id;
    Sequence sequence;
};

struct CheckpointReceipt {
    Sequence target_sequence;
    Sequence checkpoint_sequence;
    Generation manifest_generation;
};

struct EngineStats {
    Sequence published_sequence = 0;
    Sequence checkpoint_sequence = 0;
    Generation view_generation = 0;
    std::uint64_t live_vectors = 0;
    std::uint64_t memory_accounted_bytes = 0;
    std::uint64_t cache_resident_bytes = 0;
    std::uint64_t delta_resident_bytes = 0;
    std::uint64_t retired_pinned_bytes = 0;
    std::uint64_t io_reads_submitted = 0;
    std::uint64_t io_read_bytes_completed = 0;
    std::uint64_t io_read_errors = 0;
    std::uint64_t prefetch_submitted = 0;
    std::uint64_t prefetch_consumed = 0;
    std::uint64_t wal_bytes_written = 0;
    std::uint64_t checkpoint_bytes_written = 0;
    std::uint64_t merge_bytes_written = 0;
    std::uint64_t logical_update_bytes = 0;
    std::uint64_t pending_write_count = 0;
    std::uint64_t merge_backlog_bytes = 0;
};

// Declaration-only contract. No engine implementation is supplied here.
class Engine {
public:
    static Result<std::unique_ptr<Engine>> Open(
        const std::filesystem::path& directory, const EngineOptions& options);

    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    Result<SearchResult> Search(
        std::span<const float> query, const SearchOptions& options);
    Result<StoredVector> Get(VectorId id);
    Result<WriteReceipt> Insert(
        VectorId id, std::span<const float> values, const WriteOptions& options);
    Result<WriteReceipt> Upsert(
        VectorId id, std::span<const float> values, const WriteOptions& options);
    Result<CheckpointReceipt> Checkpoint(std::chrono::milliseconds timeout);
    Result<EngineStats> Stats() const;
    Result<Success> Close();

private:
    struct Impl;
    explicit Engine(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace agent_io
