#include "agent_io/benchmark_runner.hpp"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>

namespace agent_io {
namespace {
using Clock = std::chrono::steady_clock;
using Ns = std::chrono::nanoseconds;

constexpr std::uint64_t kMaxPhaseNs = 86400000000000ULL;
constexpr std::uint64_t kControlAllowance = 131072;
constexpr std::uint64_t kWorkerAllowance = 4096;

Error Invalid(const char* message) {
    return {ErrorCode::invalid_argument, message};
}

std::uint64_t PhaseNs(std::chrono::milliseconds value) {
    if (value.count() < 0 ||
        static_cast<std::uint64_t>(value.count()) > kMaxPhaseNs / 1000000)
        throw std::invalid_argument("Phase duration must be between 0 and 24 hours");
    return static_cast<std::uint64_t>(value.count()) * 1000000;
}

std::uint64_t Elapsed(Clock::time_point origin) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<Ns>(Clock::now() - origin).count());
}

std::uint64_t Scheduled(std::uint64_t ordinal, double rate) {
    const auto value = static_cast<long double>(ordinal) * 1000000000.L / rate;
    if (value >= static_cast<long double>(UINT64_MAX)) return UINT64_MAX;
    return static_cast<std::uint64_t>(value);
}

std::uint64_t Planned(std::uint64_t duration, double rate) {
    auto count = static_cast<std::uint64_t>(std::ceil(
        static_cast<long double>(duration) * rate / 1000000000.L));
    while (count > 0 && Scheduled(count - 1, rate) >= duration) --count;
    while (Scheduled(count, rate) < duration) ++count;
    return count;
}

bool ValidUtf8(std::string_view value) {
    return value.empty() || (value.size() <= INT_MAX &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), nullptr, 0) > 0);
}

class JsonLine {
    std::span<char> buffer_;
    std::size_t used_ = 0;
public:
    explicit JsonLine(std::span<char> buffer) : buffer_(buffer) {}
    void Raw(std::string_view text) {
        if (text.size() > buffer_.size() - used_)
            throw std::length_error("Record exceeds max_record_bytes");
        std::memcpy(buffer_.data() + used_, text.data(), text.size());
        used_ += text.size();
    }
    void Number(std::uint64_t value) {
        char text[24];
        const auto result = std::to_chars(text, text + sizeof(text), value);
        Raw({text, static_cast<std::size_t>(result.ptr - text)});
    }
    void Decimal(std::uint64_t value) { Raw("\""); Number(value); Raw("\""); }
    void Signed(int value) {
        char text[24];
        const auto result = std::to_chars(text, text + sizeof(text), value);
        Raw({text, static_cast<std::size_t>(result.ptr - text)});
    }
    void Double(double value) {
        char text[64];
        const auto result = std::to_chars(text, text + sizeof(text), value,
            std::chars_format::general, std::numeric_limits<double>::max_digits10);
        if (result.ec != std::errc{}) throw std::runtime_error("Distance serialization");
        Raw({text, static_cast<std::size_t>(result.ptr - text)});
    }
    void Text(std::string_view text) {
        if (!ValidUtf8(text)) throw std::runtime_error("Non-UTF-8 diagnostic or run ID");
        Raw("\"");
        for (unsigned char character : text) {
            if (character == '"' || character == '\\') {
                char pair[] = {'\\', static_cast<char>(character)};
                Raw({pair, 2});
            } else if (character < 32) {
                constexpr char hex[] = "0123456789abcdef";
                char escaped[] = {'\\', 'u', '0', '0', hex[character >> 4],
                                  hex[character & 15]};
                Raw({escaped, 6});
            } else {
                const char byte = static_cast<char>(character);
                Raw({&byte, 1});
            }
        }
        Raw("\"");
    }
    std::string_view View() const { return {buffer_.data(), used_}; }
};

std::string_view ErrorName(ErrorCode code) {
    switch (code) {
    case ErrorCode::invalid_argument: return "invalid_argument";
    case ErrorCode::not_found: return "not_found";
    case ErrorCode::already_exists: return "already_exists";
    case ErrorCode::resource_exhausted: return "resource_exhausted";
    case ErrorCode::deadline_exceeded: return "deadline_exceeded";
    case ErrorCode::search_incomplete: return "search_incomplete";
    case ErrorCode::io_error: return "io_error";
    case ErrorCode::corruption: return "corruption";
    case ErrorCode::unsupported: return "unsupported";
    case ErrorCode::closed: return "closed";
    case ErrorCode::recovery_required: return "recovery_required";
    }
    throw std::runtime_error("Unknown target error code");
}

void Header(JsonLine& line, const BenchmarkOptions& options) {
    line.Raw("{\"schema_version\":1,\"protocol_version\":\"EVAL-1.1\",\"purpose\":");
    line.Text(options.purpose);
    line.Raw(",\"run_id\":");
    line.Text(options.run_id);
}

struct Request {
    std::uint64_t ordinal = 0;
    std::uint64_t row = 0;
    std::uint64_t scheduled_ns = 0;
};

struct Slot {
    std::vector<char> record;
    std::vector<Neighbor> neighbors;
    RequestTiming timing{};
    RequestStatus status = RequestStatus::ok;
    ErrorCode error_code = ErrorCode::io_error;
    std::size_t length = 0;
};

void QueryLine(Slot& slot, const BenchmarkOptions& options, const Request& request,
               VectorId query_id, const BenchmarkSearchResult* result,
               const Error* error) {
    JsonLine line(slot.record);
    Header(line, options);
    line.Raw(",\"operation_id\":\"q-");
    line.Number(request.ordinal);
    line.Raw("\",\"status\":");
    line.Text(slot.status == RequestStatus::ok ? "ok" :
              slot.status == RequestStatus::error ? "error" : "rejected");
    line.Raw(",\"scheduled_ns\":"); line.Decimal(slot.timing.scheduled_ns);
    line.Raw(",\"api_start_ns\":");
    if (slot.timing.api_start_ns) line.Decimal(*slot.timing.api_start_ns);
    else line.Raw("null");
    line.Raw(",\"completed_ns\":"); line.Decimal(slot.timing.completed_ns);
    line.Raw(",\"error\":");
    if (!error) {
        line.Raw("null");
    } else {
        line.Raw("{\"code\":"); line.Text(ErrorName(error->code));
        line.Raw(",\"message\":");
        auto message = std::string_view(error->message);
        if (message.empty()) message = "Target returned an error";
        if (!ValidUtf8(message)) throw std::runtime_error("Non-UTF-8 target error message");
        if (message.size() > 4096) {
            message = message.substr(0, 4096);
            while (!message.empty() && !ValidUtf8(message)) message.remove_suffix(1);
        }
        line.Text(message);
        line.Raw(",\"system_error\":"); line.Signed(error->system_error);
        line.Raw(",\"write_effect\":\"none\",\"attempted_sequence\":null}");
    }
    line.Raw(",\"query_id\":"); line.Decimal(query_id);
    line.Raw(",\"snapshot_sequence\":");
    if (result) line.Decimal(result->snapshot_sequence); else line.Raw("null");
    line.Raw(",\"view_generation\":");
    if (result) line.Decimal(result->view_generation); else line.Raw("null");
    line.Raw(",\"k\":"); line.Number(options.search.k);
    line.Raw(",\"neighbors\":[");
    if (result) {
        for (std::uint32_t i = 0; i < result->neighbor_count; ++i) {
            const auto& neighbor = slot.neighbors[i];
            if (i) line.Raw(",");
            line.Raw("{\"id\":"); line.Decimal(neighbor.id);
            line.Raw(",\"version\":"); line.Decimal(neighbor.version);
            line.Raw(",\"squared_distance\":"); line.Double(neighbor.squared_distance);
            line.Raw("}");
        }
    }
    line.Raw("]}\n");
    slot.length = line.View().size();
}

bool ValidResult(const BenchmarkSearchResult& result, const Slot& slot) {
    if (!result.view_generation || result.neighbor_count > slot.neighbors.size()) return false;
    for (std::uint32_t i = 0; i < result.neighbor_count; ++i) {
        const auto& item = slot.neighbors[i];
        if (!std::isfinite(item.squared_distance) || item.squared_distance < 0 ||
            item.version > result.snapshot_sequence) return false;
        if (i) {
            const auto& previous = slot.neighbors[i - 1];
            if (previous.squared_distance > item.squared_distance ||
                (previous.squared_distance == item.squared_distance &&
                 previous.id >= item.id)) return false;
        }
        for (std::uint32_t j = 0; j < i; ++j)
            if (slot.neighbors[j].id == item.id) return false;
    }
    return true;
}

void Observe(BenchmarkLatency& histogram, std::uint64_t value) {
    if (!histogram.count || value < histogram.min_ns) histogram.min_ns = value;
    histogram.max_ns = (std::max)(histogram.max_ns, value);
    ++histogram.count;
    ++histogram.bins[std::bit_width(value)];
}

EngineStats Difference(const EngineStats& before, const EngineStats& after) {
    EngineStats delta;
    auto subtract = [](std::uint64_t first, std::uint64_t last) {
        if (last < first) throw std::runtime_error("Target Stats counter regression");
        return last - first;
    };
#define AGENT_IO_DELTA(field) delta.field = subtract(before.field, after.field)
    AGENT_IO_DELTA(io_reads_submitted);
    AGENT_IO_DELTA(io_read_bytes_completed);
    AGENT_IO_DELTA(io_read_errors);
    AGENT_IO_DELTA(prefetch_submitted);
    AGENT_IO_DELTA(prefetch_consumed);
    AGENT_IO_DELTA(wal_bytes_written);
    AGENT_IO_DELTA(checkpoint_bytes_written);
    AGENT_IO_DELTA(merge_bytes_written);
#undef AGENT_IO_DELTA
    return delta;
}

class Phase {
    const BenchmarkOptions& options_;
    BenchmarkTarget& target_;
    BenchmarkSink& sink_;
    BenchmarkReport& report_;
    const bool measured_;
    const std::uint64_t duration_;
    std::vector<Request> requests_;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> ready_, free_;
    std::vector<char> sample_buffer_;
    std::vector<std::jthread> workers_;
    std::jthread logger_;
    std::stop_source cancel_;
    std::mutex mutex_;
    std::condition_variable changed_;
    Clock::time_point origin_{};
    Clock::time_point deadline_{};
    std::size_t request_head_ = 0, request_size_ = 0;
    std::size_t ready_head_ = 0, ready_size_ = 0;
    std::size_t free_size_ = 0;
    std::uint32_t active_ = 0;
    bool started_ = false, arrivals_done_ = false, frozen_ = false;
    bool rejection_busy_ = false, logger_busy_ = false;
    bool artifact_error_ = false;
    const char* diagnostic_ = "";

    void Fail(const char* diagnostic) {
        std::lock_guard lock(mutex_);
        artifact_error_ = true;
        if (!*diagnostic_) diagnostic_ = diagnostic;
        frozen_ = true;
        changed_.notify_all();
    }

    void PublishLocked(std::uint32_t index) {
        ready_[(ready_head_ + ready_size_) % ready_.size()] = index;
        ++ready_size_;
        changed_.notify_all();
    }

    void Worker() noexcept {
        try {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [&] { return started_ || frozen_; });
            while (true) {
                changed_.wait(lock, [&] {
                    return frozen_ || (request_size_ && free_size_) ||
                           (arrivals_done_ && !request_size_);
                });
                if (frozen_ || (arrivals_done_ && !request_size_)) return;
                const auto index = free_[--free_size_];
                const auto request = requests_[request_head_];
                request_head_ = (request_head_ + 1) % requests_.size();
                --request_size_;
                ++active_;
                auto& slot = slots_[index];
                slot.status = RequestStatus::ok;
                slot.timing = {request.scheduled_ns, std::nullopt, 0};
                lock.unlock();
                Result<BenchmarkSearchResult> result = Error{
                    ErrorCode::io_error, "Target Search threw an exception"};
                try {
                    auto search = options_.search;
                    const auto remaining = std::chrono::duration_cast<
                        std::chrono::milliseconds>(deadline_ - Clock::now());
                    search.timeout = (std::min)(search.timeout,
                        (std::max)(remaining, std::chrono::milliseconds(1)));
                    slot.timing.api_start_ns = Elapsed(origin_);
                    result = target_.Search(request.row, search, slot.neighbors,
                                            cancel_.get_token());
                } catch (const std::exception& error) {
                    result = Error{ErrorCode::io_error, error.what()};
                } catch (...) {}
                if (const auto* success = std::get_if<BenchmarkSearchResult>(&result)) {
                    if (!ValidResult(*success, slot) || (measured_ &&
                        success->snapshot_sequence != report_.start_stats.published_sequence))
                        result = Error{ErrorCode::corruption, "Invalid target query result"};
                } else {
                    const auto& error = std::get<Error>(result);
                    if (error.write_effect != WriteEffect::none || error.attempted_sequence)
                        result = Error{ErrorCode::corruption, "Query target returned write effect"};
                }
                slot.timing.completed_ns = Elapsed(origin_);
                const auto* error = std::get_if<Error>(&result);
                slot.status = error ? RequestStatus::error : RequestStatus::ok;
                if (error) slot.error_code = error->code;
                QueryLine(slot, options_, request, target_.QueryId(request.row),
                          std::get_if<BenchmarkSearchResult>(&result), error);
                lock.lock();
                --active_;
                if (!frozen_ && Clock::now() <= deadline_) PublishLocked(index);
                else free_[free_size_++] = index;
                changed_.notify_all();
            }
        } catch (...) {
            Fail("Worker could not form a complete query record");
        }
    }

    bool Sample(std::uint64_t sampled_ns) {
        JsonLine line(sample_buffer_);
        PROCESS_MEMORY_COUNTERS_EX process{};
        process.cb = sizeof(process);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION job{};
        const bool process_ok = GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process)) != 0;
        const bool job_ok = QueryInformationJobObject(nullptr,
            JobObjectExtendedLimitInformation, &job, sizeof(job), nullptr) != 0;
        Header(line, options_);
        line.Raw(",\"sampled_ns\":"); line.Decimal(sampled_ns);
        line.Raw(",\"private_bytes\":");
        if (process_ok) line.Decimal(process.PrivateUsage); else line.Raw("null");
        line.Raw(",\"working_set_bytes\":");
        if (process_ok) line.Decimal(process.WorkingSetSize); else line.Raw("null");
        line.Raw(",\"kernel_peak_job_commit_bytes\":");
        if (job_ok) line.Decimal(job.PeakJobMemoryUsed); else line.Raw("null");
        line.Raw(",\"unavailable_reason\":");
        if (process_ok && job_ok) line.Raw("null");
        else line.Text("Process memory or current Job Object measurement unavailable");
        line.Raw("}\n");
        if (!sink_.WriteSample(line.View())) return false;
        report_.sample_log_bytes += line.View().size();
        return true;
    }

    void Count(const Slot& slot) {
        auto& counts = report_.query;
        if (slot.status == RequestStatus::ok) ++counts.succeeded;
        else if (slot.status == RequestStatus::error) ++counts.failed;
        else ++counts.rejected;
        if (slot.timing.completed_ns < duration_) {
            ++counts.window_completed;
            if (slot.status == RequestStatus::ok) ++counts.window_succeeded;
        }
        const auto status = static_cast<std::size_t>(slot.status);
        Observe(report_.end_to_end[status],
                slot.timing.completed_ns - slot.timing.scheduled_ns);
        if (slot.timing.api_start_ns) {
            Observe(report_.queue[status],
                    *slot.timing.api_start_ns - slot.timing.scheduled_ns);
            Observe(report_.service[status],
                    slot.timing.completed_ns - *slot.timing.api_start_ns);
        }
        if (slot.status == RequestStatus::error) {
            ++report_.errors_by_code[static_cast<std::size_t>(slot.error_code)];
            if (slot.error_code == ErrorCode::deadline_exceeded) {
                Observe(report_.timeout_end_to_end,
                        slot.timing.completed_ns - slot.timing.scheduled_ns);
                Observe(report_.timeout_queue,
                        *slot.timing.api_start_ns - slot.timing.scheduled_ns);
                Observe(report_.timeout_service,
                        slot.timing.completed_ns - *slot.timing.api_start_ns);
            }
        }
    }

    void Logger() noexcept {
        try {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [&] { return started_ || frozen_; });
            if (!started_) return;
            auto next_sample = measured_ ? origin_ : deadline_;
            while (true) {
                if (measured_ && !frozen_ && Clock::now() >= next_sample) {
                    const auto sampled_ns = Elapsed(origin_);
                    logger_busy_ = true;
                    lock.unlock();
                    const bool ok = Sample(sampled_ns);
                    lock.lock();
                    logger_busy_ = false;
                    if (!ok) {
                        artifact_error_ = true;
                        diagnostic_ = "Resource sample output failed";
                        frozen_ = true;
                    }
                    next_sample = Clock::now() + options_.sample_interval;
                    changed_.notify_all();
                }
                if (ready_size_) {
                    const auto index = ready_[ready_head_];
                    ready_head_ = (ready_head_ + 1) % ready_.size();
                    --ready_size_;
                    logger_busy_ = true;
                    const auto& slot = slots_[index];
                    lock.unlock();
                    const bool ok = !measured_ || sink_.WriteQuery(
                        {slot.record.data(), slot.length});
                    lock.lock();
                    if (ok) {
                        Count(slot);
                        if (measured_) report_.query_log_bytes += slot.length;
                    } else {
                        artifact_error_ = true;
                        diagnostic_ = "Query log output failed";
                        frozen_ = true;
                    }
                    if (index == 0) rejection_busy_ = false;
                    else free_[free_size_++] = index;
                    logger_busy_ = false;
                    changed_.notify_all();
                } else if (frozen_) {
                    lock.unlock();
                    if (measured_ && !sink_.Flush()) Fail("Streaming artifact flush failed");
                    return;
                } else {
                    changed_.wait_until(lock, next_sample);
                }
            }
        } catch (...) {
            Fail("Logger could not form a complete output record");
        }
    }

public:
    Clock::time_point Deadline() const { return deadline_; }
    std::uint64_t ElapsedNs() const { return Elapsed(origin_); }

    Phase(const BenchmarkOptions& options, BenchmarkTarget& target,
          BenchmarkSink& sink, BenchmarkReport& report, bool measured,
          std::uint64_t duration)
        : options_(options), target_(target), sink_(sink), report_(report),
          measured_(measured), duration_(duration),
          requests_(report.resources.client_slots),
          slots_(report.resources.result_slots),
          ready_(slots_.size()), free_(slots_.size()), sample_buffer_(4096) {
        for (auto& slot : slots_) {
            slot.record.resize(options.limits.max_record_bytes);
            slot.neighbors.resize(options.search.k);
        }
        for (std::uint32_t i = 1; i < slots_.size(); ++i) free_[free_size_++] = i;
        workers_.reserve(options.limits.workers);
    }

    void Run() {
        try {
            logger_ = std::jthread([this] { Logger(); });
            for (std::uint32_t i = 0; i < options_.limits.workers; ++i)
                workers_.emplace_back([this] { Worker(); });
        } catch (...) {
            {
                std::lock_guard lock(mutex_);
                frozen_ = true;
                changed_.notify_all();
            }
            for (auto& worker : workers_) if (worker.joinable()) worker.join();
            if (logger_.joinable()) logger_.join();
            throw;
        }
        std::unique_lock lock(mutex_);
        origin_ = Clock::now();
        deadline_ = origin_ + Ns(duration_ + PhaseNs(options_.drain_timeout));
        const auto total = Planned(duration_, options_.target_ops_per_second);
        report_.query.scheduled = total;
        started_ = true;
        changed_.notify_all();
        for (std::uint64_t ordinal = 0; ordinal < total && !frozen_; ++ordinal) {
            const auto scheduled = Scheduled(ordinal, options_.target_ops_per_second);
            changed_.wait_until(lock, origin_ + Ns(scheduled), [&] { return frozen_; });
            if (frozen_) break;
            if (Clock::now() >= deadline_) break;
            Request request{ordinal,
                (options_.seed % options_.query_count + ordinal % options_.query_count) %
                    options_.query_count, scheduled};
            if (request_size_ < requests_.size()) {
                requests_[(request_head_ + request_size_) % requests_.size()] = request;
                ++request_size_;
                changed_.notify_all();
            } else if (!rejection_busy_) {
                rejection_busy_ = true;
                auto& slot = slots_[0];
                slot.status = RequestStatus::rejected;
                slot.timing = {scheduled, std::nullopt, Elapsed(origin_)};
                const Error error{ErrorCode::resource_exhausted, "Client queue is full"};
                try {
                    QueryLine(slot, options_, request, target_.QueryId(request.row),
                              nullptr, &error);
                    PublishLocked(0);
                } catch (...) {
                    artifact_error_ = true;
                    diagnostic_ = "Rejection record serialization failed";
                    frozen_ = true;
                }
            } else {
                artifact_error_ = true;
                diagnostic_ = "Dedicated rejection slot is busy; arrival clock was not delayed";
                frozen_ = true;
            }
        }
        arrivals_done_ = true;
        changed_.notify_all();
        if (!frozen_) {
            changed_.wait_until(lock, origin_ + Ns(duration_), [&] { return frozen_; });
            changed_.wait_until(lock, deadline_, [&] {
                return frozen_ || (!request_size_ && !active_ && !ready_size_ && !logger_busy_);
            });
        }
        const bool timed_out = request_size_ || active_ || ready_size_ || logger_busy_;
        const auto cutoff = Elapsed(origin_);
        frozen_ = true;
        changed_.notify_all();
        lock.unlock();
        cancel_.request_stop();
        for (auto& worker : workers_) worker.join();
        logger_.join();
        report_.duration_ns = duration_;
        report_.drain_ns = cutoff > duration_ ? cutoff - duration_ : 0;
        report_.cleanup_ns = Elapsed(origin_) > cutoff ? Elapsed(origin_) - cutoff : 0;
        report_.query.pending = total - report_.query.succeeded -
                                report_.query.failed - report_.query.rejected;
        report_.run_incomplete = artifact_error_ || timed_out || report_.query.pending;
        report_.exit_code = artifact_error_ ? 5 : report_.run_incomplete ? 4 : 0;
        if (*diagnostic_) report_.diagnostic = diagnostic_;
        else if (report_.run_incomplete) report_.diagnostic = "Client drain deadline exceeded";
    }
};

}  // namespace

Result<BenchmarkResourcePlan> PlanStaticBenchmark(const BenchmarkOptions& options) {
    try {
        if (options.run_id.empty() || options.run_id.size() > 128 ||
            !ValidUtf8(options.run_id) ||
            (options.purpose != "measurement" && options.purpose != "contract_example"))
            return Invalid("Invalid run ID or purpose");
        const auto& limits = options.limits;
        if (!options.query_count || !limits.workers || !limits.client_queue_capacity ||
            !limits.result_queue_capacity || !limits.max_record_bytes ||
            options.query_count > UINT64_MAX / 2)
            return Invalid("Query count, workers and queue capacities must be positive and bounded");
        if (!std::isfinite(options.target_ops_per_second) ||
            options.target_ops_per_second <= 0 || options.target_ops_per_second > 1e9)
            return Invalid("Arrival rate must be finite and in (0, 1e9]");
        if (!options.search.k || options.search.candidate_limit < options.search.k ||
            options.search.timeout.count() <= 0)
            return Invalid("Invalid search limits");
        const std::uint64_t record_bound = 16384 + 6 * 4096 +
                                          std::uint64_t{128} * options.search.k;
        if (record_bound > limits.max_record_bytes ||
            limits.client_queue_bytes < limits.max_record_bytes ||
            limits.result_queue_bytes < limits.max_record_bytes)
            return Invalid("Record or queue byte budget is too small for K");
        const auto warmup = PhaseNs(options.warmup);
        const auto duration = PhaseNs(options.duration);
        const auto drain = PhaseNs(options.drain_timeout);
        const auto sample = PhaseNs(options.sample_interval);
        if (!duration || !drain || !sample || duration + drain > kMaxPhaseNs ||
            warmup + drain > kMaxPhaseNs)
            return Invalid("Duration, drain and sampling must be positive; phase <= 24 hours");
        const auto per_slot = limits.max_record_bytes +
            std::uint64_t{sizeof(Neighbor)} * options.search.k + sizeof(Slot) +
            2 * sizeof(std::uint32_t) + 128;
        BenchmarkResourcePlan plan;
        plan.client_slots = static_cast<std::uint32_t>((std::min)(
            std::uint64_t{limits.client_queue_capacity},
            limits.client_queue_bytes / (sizeof(Request) + 64)));
        plan.result_slots = static_cast<std::uint32_t>((std::min)(
            std::uint64_t{limits.result_queue_capacity},
            limits.result_queue_bytes / per_slot));
        if (!plan.client_slots || plan.result_slots <= limits.workers)
            return Invalid("Result pool needs one slot per worker plus a dedicated rejection slot");
        plan.workspace_bytes = kControlAllowance +
            std::uint64_t{plan.client_slots} * (sizeof(Request) + 64) +
            std::uint64_t{plan.result_slots} * per_slot +
            std::uint64_t{limits.workers} * kWorkerAllowance;
        return plan;
    } catch (const std::invalid_argument& error) {
        return Error{ErrorCode::invalid_argument, error.what()};
    }
}

Result<BenchmarkReport> RunStaticBenchmark(
    const BenchmarkOptions& options, BenchmarkTarget& target,
    BenchmarkSink& sink, std::shared_ptr<ResourceGovernor> governor) {
    auto planned = PlanStaticBenchmark(options);
    if (const auto* error = std::get_if<Error>(&planned)) return *error;
    if (!governor) return Invalid("Missing workspace governor");
    auto reserved = governor->TryReserve(
        {MemoryRegion::workspace, std::get<BenchmarkResourcePlan>(planned).workspace_bytes});
    if (const auto* error = std::get_if<Error>(&reserved)) return *error;
    // Reservation precedes Phase allocations and is destroyed after all phases.
    auto reservation = std::get<std::shared_ptr<const BudgetReservation>>(std::move(reserved));
    try {
        BenchmarkReport report;
        report.resources = std::get<BenchmarkResourcePlan>(planned);
        if (options.warmup.count()) {
            BenchmarkReport warmup;
            warmup.resources = report.resources;
            const auto start = Clock::now();
            {
                Phase phase(options, target, sink, warmup, false, PhaseNs(options.warmup));
                phase.Run();
                const auto drained = target.Drain(phase.Deadline());
                if (const auto* error = std::get_if<Error>(&drained)) return *error;
                if (Clock::now() > phase.Deadline())
                    return Error{ErrorCode::deadline_exceeded, "Warmup I/O drain deadline exceeded"};
            }
            if (warmup.run_incomplete)
                return Error{ErrorCode::deadline_exceeded, "Warmup did not drain"};
            report.warmup_elapsed_ns = Elapsed(start);
            report.warmup_query = warmup.query;
        }
        auto before = target.Stats();
        if (const auto* error = std::get_if<Error>(&before)) return *error;
        report.start_stats = std::get<EngineStats>(before);
        {
            Phase phase(options, target, sink, report, true, PhaseNs(options.duration));
            phase.Run();
            const auto drain_start = Clock::now();
            const auto drained = target.Drain(phase.Deadline());
            if (report.run_incomplete) {
                report.cleanup_ns += Elapsed(drain_start);
            } else {
                report.drain_ns = phase.ElapsedNs() - report.duration_ns;
                report.cleanup_ns = 0;
            }
            if (const auto* error = std::get_if<Error>(&drained)) {
                report.run_incomplete = true;
                if (report.exit_code != 5) report.exit_code = 4;
                report.diagnostic = error->message;
            } else if (Clock::now() > phase.Deadline()) {
                report.run_incomplete = true;
                if (report.exit_code != 5) report.exit_code = 4;
                if (report.diagnostic.empty()) report.diagnostic = "I/O drain deadline exceeded";
            }
        }
        auto after = target.Stats();
        if (const auto* error = std::get_if<Error>(&after)) return *error;
        report.end_stats = std::get<EngineStats>(after);
        report.counter_delta = Difference(report.start_stats, report.end_stats);
        if (report.start_stats.published_sequence != report.end_stats.published_sequence ||
            report.counter_delta.wal_bytes_written || report.counter_delta.merge_bytes_written ||
            report.counter_delta.checkpoint_bytes_written)
            return Invalid("Static benchmark target changed published sequence or wrote data");
        return report;
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::resource_exhausted, "Benchmark allocation failed"};
    } catch (const std::exception& error) {
        return Error{ErrorCode::io_error, error.what()};
    }
}

}  // namespace agent_io
