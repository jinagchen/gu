#pragma once

#include "benchmark.hpp"
#include "storage.hpp"

#include <array>
#include <stop_token>
#include <string_view>

namespace agent_io {

struct BenchmarkOptions {
    std::string run_id;
    std::string purpose = "measurement";
    BenchmarkLimits limits;
    SearchOptions search;
    std::uint64_t query_count = 0;
    std::uint64_t seed = 0;
    double target_ops_per_second = 100;
    std::chrono::milliseconds warmup{0};
    std::chrono::milliseconds duration{1000};
    std::chrono::milliseconds drain_timeout{5000};
    std::chrono::milliseconds sample_interval{1000};
};

struct BenchmarkSearchResult {
    Sequence snapshot_sequence = 0;
    Generation view_generation = 1;
    std::uint32_t neighbor_count = 0;
};

enum class BenchmarkOperation { query, insert, upsert };

struct BenchmarkEvent {
    BenchmarkOperation operation;
    std::string operation_id;
    std::uint64_t row = 0;
    VectorId id = 0;
};

struct BenchmarkTrace {
    std::span<const BenchmarkEvent> events;
    std::string_view payload_file;
    std::string_view payload_sha256;
    std::uint32_t dimension = 0;
};

// The caller owns the target and all query inputs until RunStaticBenchmark
// returns. Search is concurrent; it borrows the preallocated output only for
// the call. Target allocations and input storage require their own budgets.
class BenchmarkTarget {
public:
    virtual ~BenchmarkTarget() = default;
    virtual VectorId QueryId(std::uint64_t row) const noexcept = 0;
    virtual Result<BenchmarkSearchResult> Search(
        std::uint64_t row, const SearchOptions& options,
        std::span<Neighbor> output, std::stop_token stop) = 0;
    virtual Result<WriteReceipt> Write(const BenchmarkEvent&, const WriteOptions&) {
        return Error{ErrorCode::unsupported, "Benchmark target does not accept writes"};
    }
    virtual Result<EngineStats> Stats() const = 0;
    // Quiesce outstanding I/O without closing admission; called after warmup
    // and after measurement, with the same deadline as the client drain.
    virtual Result<Success> Drain(
        std::chrono::steady_clock::time_point deadline) = 0;
};

// Only the logger calls these methods. Views expire when the method returns.
// Returning false means artifacts are incomplete; there is no fallback queue.
class BenchmarkSink {
public:
    virtual ~BenchmarkSink() = default;
    virtual bool WriteQuery(std::string_view record) noexcept = 0;
    virtual bool WriteUpdate(std::string_view, const WriteReceipt*) noexcept { return false; }
    virtual bool WriteSample(std::string_view record) noexcept = 0;
    virtual bool Flush() noexcept { return true; }
};

struct BenchmarkResourcePlan {
    std::uint32_t client_slots = 0;
    std::uint32_t result_slots = 0;
    std::uint64_t workspace_bytes = 0;
};

struct BenchmarkLatency {
    std::uint64_t count = 0;
    std::uint64_t min_ns = 0;
    std::uint64_t max_ns = 0;
    // Bin 0 contains 0 ns; bin i contains [2^(i-1), 2^i-1] ns.
    std::array<std::uint64_t, 65> bins{};
};

struct BenchmarkReport {
    RequestCounts query;
    RequestCounts insert;
    RequestCounts upsert;
    EngineStats start_stats;
    EngineStats end_stats;
    EngineStats counter_delta;
    BenchmarkResourcePlan resources;
    std::uint64_t duration_ns = 0;
    std::uint64_t drain_ns = 0;
    std::uint64_t cleanup_ns = 0;
    std::uint64_t warmup_elapsed_ns = 0;
    RequestCounts warmup_query;
    std::uint64_t query_log_bytes = 0;
    std::uint64_t write_log_bytes = 0;
    std::uint64_t sample_log_bytes = 0;
    bool run_incomplete = false;
    bool requires_review = false;
    int exit_code = 0;
    std::string diagnostic;
    // Indexed by RequestStatus: ok, error, rejected.
    std::array<BenchmarkLatency, 3> end_to_end;
    std::array<BenchmarkLatency, 3> queue;
    std::array<BenchmarkLatency, 3> service;
    std::array<BenchmarkLatency, 2> write_end_to_end;
    std::array<BenchmarkLatency, 2> write_queue;
    std::array<BenchmarkLatency, 2> write_service;
    std::array<std::uint64_t, 11> errors_by_code{};
    BenchmarkLatency timeout_end_to_end;
    BenchmarkLatency timeout_queue;
    BenchmarkLatency timeout_service;
};

Result<BenchmarkResourcePlan> PlanStaticBenchmark(const BenchmarkOptions& options);

// Static queries only. Warmup is drained before taking the initial Stats.
// A timeout freezes publication, requests cooperative cancellation, and joins
// all threads before returning. An uncooperative target needs an outer process
// timeout; no thread may outlive its target or input buffers.
Result<BenchmarkReport> RunStaticBenchmark(
    const BenchmarkOptions& options, BenchmarkTarget& target,
    BenchmarkSink& sink, std::shared_ptr<ResourceGovernor> governor);

Result<BenchmarkReport> RunMixedBenchmark(
    const BenchmarkOptions& options, const BenchmarkTrace& trace, BenchmarkTarget& target,
    BenchmarkSink& sink, std::shared_ptr<ResourceGovernor> governor);

}  // namespace agent_io
