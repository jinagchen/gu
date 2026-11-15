#include "agent_io/benchmark_runner.hpp"
#include "agent_io/synchronous_storage.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace agent_io;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class T> T Take(Result<T> result) {
    if (const auto* error = std::get_if<Error>(&result))
        throw std::runtime_error(error->message);
    return std::get<T>(std::move(result));
}

MemoryBudget Budget() {
    return {16777216, 2097152, 2097152, 2097152, 8388608, 2097152};
}

BenchmarkOptions Options() {
    BenchmarkOptions options;
    options.run_id = "benchmark-test";
    options.purpose = "contract_example";
    options.query_count = 7;
    options.seed = UINT64_MAX;
    options.duration = 40ms;
    options.drain_timeout = 1000ms;
    options.sample_interval = 10ms;
    options.target_ops_per_second = 500;
    options.limits.workers = 3;
    return options;
}

class Target final : public BenchmarkTarget {
    mutable std::atomic<std::uint32_t> stats_calls_{0};
public:
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint32_t> active{0}, maximum{0};
    std::chrono::milliseconds delay{0};
    bool fail = false, malformed = false, regress = false, write = false;
    ErrorCode failure_code = ErrorCode::io_error;
    std::uint32_t rendezvous = 0;
    VectorId QueryId(std::uint64_t row) const noexcept override { return UINT64_MAX - row; }
    Result<BenchmarkSearchResult> Search(
        std::uint64_t, const SearchOptions&, std::span<Neighbor> output,
        std::stop_token stop) override {
        ++calls;
        const auto current = active.fetch_add(1) + 1;
        auto old = maximum.load();
        while (old < current && !maximum.compare_exchange_weak(old, current)) {}
        const auto until = Clock::now() + delay;
        const auto rendezvous_deadline = Clock::now() + 500ms;
        while (!stop.stop_requested() && (Clock::now() < until ||
               (maximum.load() < rendezvous && Clock::now() < rendezvous_deadline)))
            std::this_thread::sleep_for(1ms);
        --active;
        if (stop.stop_requested())
            return Error{ErrorCode::deadline_exceeded, "Canceled query"};
        if (fail)
            return Error{failure_code, "quoted \"failure\"\nwith \\ slash", 123};
        for (std::uint32_t i = 0; i < output.size(); ++i)
            output[i] = {malformed ? 42 : i, 0, static_cast<double>(i)};
        return BenchmarkSearchResult{0, 1, static_cast<std::uint32_t>(output.size())};
    }
    Result<EngineStats> Stats() const override {
        EngineStats stats;
        const auto ordinal = stats_calls_.fetch_add(1);
        stats.io_reads_submitted = regress ? (ordinal == 0 ? 100 : 0) : calls.load();
        stats.io_read_bytes_completed = stats.io_reads_submitted * 4096;
        if (write && ordinal) stats.published_sequence = 1;
        return stats;
    }
    Result<Success> Drain(Clock::time_point) override {
        Require(active == 0, "Target drained before workers joined");
        return Success{};
    }
};

class Sink final : public BenchmarkSink {
public:
    std::vector<std::string> rows, samples;
    bool fail_queries = false, fail_samples = false, fail_flush = false;
    bool WriteQuery(std::string_view record) noexcept override {
        if (fail_queries) return false;
        try { rows.emplace_back(record); return true; } catch (...) { return false; }
    }
    bool WriteSample(std::string_view record) noexcept override {
        if (fail_samples) return false;
        try { samples.emplace_back(record); return true; } catch (...) { return false; }
    }
    bool Flush() noexcept override { return !fail_flush; }
};

void Conservation(const BenchmarkReport& report, const Sink& sink) {
    const auto& counts = report.query;
    Require(counts.scheduled == counts.succeeded + counts.failed + counts.rejected +
            counts.pending, "Request conservation failed");
    Require(sink.rows.size() == counts.succeeded + counts.failed + counts.rejected,
            "Terminal record count mismatch");
    Require(counts.window_succeeded <= counts.window_completed &&
            counts.window_completed <= sink.rows.size(), "Window accounting failed");
}

void NormalAndWarmup() {
    auto options = Options();
    options.warmup = 20ms;
    Target target;
    target.rendezvous = 3;
    Sink sink;
    auto governor = Take(CreateResourceGovernor(Budget()));
    auto report = Take(RunStaticBenchmark(options, target, sink, governor));
    Require(report.exit_code == 0 && !report.run_incomplete, "Normal run incomplete");
    Require(report.query.scheduled == 20 && report.query.succeeded == 20,
            "Incorrect measured request count");
    Require(report.start_stats.io_reads_submitted == 10 &&
            report.counter_delta.io_reads_submitted == 20, "Warmup leaked into Stats delta");
    Require(target.maximum >= 3 && target.active == 0, "Concurrent workers not observed");
    Require(report.warmup_elapsed_ns >= 20000000 && !sink.samples.empty(), "Missing phases/samples");
    Require(report.end_to_end[0].count == 20 && report.service[2].count == 0,
            "Latency aggregation mismatch");
    Conservation(report, sink);
    Require(sink.rows.front().find("\"184467440737095516") != std::string::npos,
            "uint64 query ID was not serialized as a decimal string");
    auto all_workspace = Take(governor->TryReserve({MemoryRegion::workspace, Budget().workspace_bytes}));
    Require(all_workspace != nullptr, "Runner reservation leaked");
}

void ErrorsAndInvalidResults() {
    auto options = Options();
    options.duration = 10ms;
    Target target;
    target.fail = true;
    Sink sink;
    auto report = Take(RunStaticBenchmark(options, target, sink,
        Take(CreateResourceGovernor(Budget()))));
    Require(report.exit_code == 0 && report.query.failed == 5, "API errors hidden");
    Require(sink.rows.front().find("\\\"failure\\\"\\u000awith \\\\ slash") !=
            std::string::npos, "Error JSON escaping failed");
    Conservation(report, sink);
    target.failure_code = ErrorCode::deadline_exceeded;
    Sink timeouts;
    report = Take(RunStaticBenchmark(options, target, timeouts,
        Take(CreateResourceGovernor(Budget()))));
    Require(report.query.failed == 5 && report.timeout_end_to_end.count == 5 &&
            report.timeout_service.count == 5 && report.errors_by_code[
                static_cast<std::size_t>(ErrorCode::deadline_exceeded)] == 5,
            "API timeouts were not classified separately");
    target.fail = false;
    target.malformed = true;
    Sink invalid_sink;
    report = Take(RunStaticBenchmark(options, target, invalid_sink,
        Take(CreateResourceGovernor(Budget()))));
    Require(report.query.failed == 5 && invalid_sink.rows.front().find(
        "\"code\":\"corruption\"") != std::string::npos, "Invalid target result was published");
}

void ClientRejections() {
    auto options = Options();
    options.duration = 300ms;
    options.target_ops_per_second = 20;
    options.limits.workers = 1;
    options.limits.client_queue_capacity = 1;
    Target target;
    target.delay = 160ms;
    Sink sink;
    const auto report = Take(RunStaticBenchmark(options, target, sink,
        Take(CreateResourceGovernor(Budget()))));
    Require(report.exit_code == 0 && report.query.scheduled == 6 &&
            report.query.rejected > 0, "Client overload was not recorded");
    Require(report.query.succeeded == target.calls, "Rejected request entered API");
    for (const auto& row : sink.rows) {
        if (row.find("\"status\":\"rejected\"") != std::string::npos)
            Require(row.find("\"api_start_ns\":null") != std::string::npos,
                    "Rejection fabricated an API time");
    }
    Conservation(report, sink);
}

void DeadlineAndOutputFailures() {
    auto options = Options();
    options.duration = 10ms;
    options.drain_timeout = 10ms;
    Target target;
    target.delay = 200ms;
    Sink sink;
    const auto before = Clock::now();
    auto report = Take(RunStaticBenchmark(options, target, sink,
        Take(CreateResourceGovernor(Budget()))));
    Require(report.exit_code == 4 && report.run_incomplete && report.query.pending > 0,
            "Drain timeout was hidden");
    Require(target.active == 0 && Clock::now() - before < 1s, "Canceled workers leaked");
    Conservation(report, sink);
    target.delay = 0ms;
    options.drain_timeout = 1000ms;
    for (int failure = 0; failure < 3; ++failure) {
        Sink broken;
        broken.fail_queries = failure == 0;
        broken.fail_samples = failure == 1;
        broken.fail_flush = failure == 2;
        report = Take(RunStaticBenchmark(options, target, broken,
            Take(CreateResourceGovernor(Budget()))));
        Require(report.exit_code == 5 && report.run_incomplete, "Log failure reported success");
        Conservation(report, broken);
    }
}

class BlockingSink final : public BenchmarkSink {
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false, released_ = false;
public:
    bool WriteQuery(std::string_view) noexcept override {
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [&] { return released_; });
        return true;
    }
    bool WriteSample(std::string_view) noexcept override { return true; }
    bool Wait() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 2s, [&] { return entered_; });
    }
    void Release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }
};

void LoggerBackpressure() {
    auto options = Options();
    options.limits.workers = 1;
    options.limits.client_queue_capacity = 1;
    options.limits.result_queue_capacity = 2;
    options.target_ops_per_second = 1000;
    options.duration = 50ms;
    BlockingSink sink;
    Target target;
    auto future = std::async(std::launch::async, [&] {
        return RunStaticBenchmark(options, target, sink, Take(CreateResourceGovernor(Budget())));
    });
    const bool entered = sink.Wait();
    std::this_thread::sleep_for(30ms);
    const auto calls_while_blocked = target.calls.load();
    sink.Release();
    const auto report = Take(future.get());
    Require(entered && calls_while_blocked == 1,
            "Worker entered API without a free result reservation");
    Require(report.exit_code == 5 && report.run_incomplete && report.query.pending > 0,
            "Rejected log saturation delayed arrivals or silently dropped records");
}

void ValidationAndBudgets() {
    auto options = Options();
    options.limits.result_queue_capacity = options.limits.workers;
    Require(std::holds_alternative<Error>(PlanStaticBenchmark(options)),
            "Missing rejection slot was accepted");
    options = Options();
    options.limits.max_record_bytes = 1024;
    Require(std::holds_alternative<Error>(PlanStaticBenchmark(options)),
            "Oversized K record was accepted");
    options = Options();
    auto budget = Budget();
    budget.workspace_bytes = 4096;
    budget.reserve_bytes += Budget().workspace_bytes - budget.workspace_bytes;
    Target target;
    Sink sink;
    const auto result = RunStaticBenchmark(options, target, sink,
        Take(CreateResourceGovernor(budget)));
    Require(std::holds_alternative<Error>(result) &&
            std::get<Error>(result).code == ErrorCode::resource_exhausted &&
            target.calls == 0, "Workspace budget was bypassed");
    options.target_ops_per_second = 1e-300;
    options.duration = 1ms;
    const auto tiny_rate = Take(RunStaticBenchmark(options, target, sink,
        Take(CreateResourceGovernor(Budget()))));
    Require(tiny_rate.query.scheduled == 1, "Tiny finite rate overflow");
    options = Options();
    Target regressing;
    regressing.regress = true;
    const auto regression = RunStaticBenchmark(options, regressing, sink,
        Take(CreateResourceGovernor(Budget())));
    Require(std::holds_alternative<Error>(regression), "Counter regression was hidden");
    Target writing;
    writing.write = true;
    Require(std::holds_alternative<Error>(RunStaticBenchmark(options, writing, sink,
        Take(CreateResourceGovernor(Budget())))), "Static run accepted a write");
}
}  // namespace

int main() {
    try {
        NormalAndWarmup();
        ErrorsAndInvalidResults();
        ClientRejections();
        DeadlineAndOutputFailures();
        LoggerBackpressure();
        ValidationAndBudgets();
        std::puts("PASS: phases, concurrency, admission, logging, deadlines, budgets and Stats");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "benchmark_runner_test failed: %s\n", error.what());
        return 1;
    }
}
