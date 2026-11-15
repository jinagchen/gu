#include "config.hpp"
#include "agent_io/synchronous_storage.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <thread>

#ifndef AGENT_IO_BUILD_COMMIT
#define AGENT_IO_BUILD_COMMIT "unavailable"
#endif
#ifndef AGENT_IO_BUILD_DIRTY
#define AGENT_IO_BUILD_DIRTY "unavailable"
#endif
#ifndef AGENT_IO_BUILD_CONFIG
#define AGENT_IO_BUILD_CONFIG "unavailable"
#endif

namespace {
using namespace agent_io;
using namespace agent_io::bench_cli;
namespace json = boost::json;
using Clock = std::chrono::steady_clock;

template<class T> T Take(Result<T> result) {
    if (const auto* error = std::get_if<Error>(&result))
        throw std::runtime_error(error->message);
    return std::get<T>(std::move(result));
}

struct Arguments {
    std::filesystem::path config, output, job_metrics;
    std::wstring target = L"engine";
    std::uint32_t delay_ms = 0, error_every = 0;
    bool test_flags = false;
};

std::uint32_t OptionNumber(const wchar_t* text) {
    const std::wstring value(text);
    if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
        throw ConfigError("Test option must be a decimal integer");
    const auto parsed = std::stoull(value);
    if (parsed > 60000) throw ConfigError("Test option exceeds 60000");
    return static_cast<std::uint32_t>(parsed);
}

Arguments ParseArguments(int argc, wchar_t** argv) {
    Arguments options;
    bool config_seen = false, output_seen = false, target_seen = false;
    bool job_seen = false, delay_seen = false, error_seen = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring name(argv[i]);
        if (i + 1 == argc) throw ConfigError("Missing argument value");
        const auto* value = argv[++i];
        auto once = [](bool& seen) {
            if (seen) throw ConfigError("Repeated command-line option");
            seen = true;
        };
        if (name == L"--config") { once(config_seen); options.config = value; }
        else if (name == L"--output") { once(output_seen); options.output = value; }
        else if (name == L"--target") { once(target_seen); options.target = value; }
        else if (name == L"--job-metrics") { once(job_seen); options.job_metrics = value; }
        else if (name == L"--test-delay-ms") {
            once(delay_seen); options.delay_ms = OptionNumber(value); options.test_flags = true;
        } else if (name == L"--test-error-every") {
            once(error_seen); options.error_every = OptionNumber(value); options.test_flags = true;
        } else throw ConfigError("Unknown command-line option");
    }
    if (!config_seen || !output_seen || options.config.empty() || options.output.empty())
        throw ConfigError("Usage: vector_bench --config run.json --output new-directory "
                          "[--target contract-test] [--job-metrics metrics.json]");
    if (options.target != L"engine" && options.target != L"contract-test")
        throw ConfigError("Unknown benchmark target");
    if (options.target != L"contract-test" && options.test_flags)
        throw ConfigError("Test flags require --target contract-test");
    return options;
}

class ContractTarget final : public BenchmarkTarget {
    std::uint64_t vector_count_;
    std::uint32_t delay_ms_, error_every_;
    std::atomic<std::uint64_t> calls_{0};
public:
    ContractTarget(const Config& config, const Arguments& arguments)
        : vector_count_(config.vector_count), delay_ms_(arguments.delay_ms),
          error_every_(arguments.error_every) {}
    VectorId QueryId(std::uint64_t row) const noexcept override { return row; }
    Result<BenchmarkSearchResult> Search(
        std::uint64_t, const SearchOptions& options, std::span<Neighbor> output,
        std::stop_token stop) override {
        const auto ordinal = calls_.fetch_add(1) + 1;
        const auto end = Clock::now() + std::chrono::milliseconds(delay_ms_);
        const auto deadline = Clock::now() + options.timeout;
        while (Clock::now() < end) {
            if (stop.stop_requested() || Clock::now() >= deadline)
                return Error{ErrorCode::deadline_exceeded, "Contract target canceled or timed out"};
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (error_every_ && ordinal % error_every_ == 0)
            return Error{ErrorCode::io_error, "Injected contract target query error"};
        const auto count = static_cast<std::uint32_t>((std::min)(
            vector_count_, std::uint64_t{options.k}));
        for (std::uint32_t i = 0; i < count; ++i)
            output[i] = {i, 0, static_cast<double>(i) * i};
        return BenchmarkSearchResult{0, 1, count};
    }
    Result<EngineStats> Stats() const override { return EngineStats{}; }
    Result<Success> Drain(Clock::time_point) override { return Success{}; }
};

class Files final : public BenchmarkSink {
    std::ofstream queries_, samples_;
public:
    explicit Files(const std::filesystem::path& directory)
        : queries_(directory / "queries.jsonl", std::ios::binary),
          samples_(directory / "samples.jsonl", std::ios::binary) {
        if (!queries_ || !samples_) throw std::runtime_error("Open streaming artifacts failed");
        std::ofstream writes(directory / "writes.jsonl", std::ios::binary);
        writes.close();
        if (!writes) throw std::runtime_error("Create writes.jsonl failed");
    }
    bool WriteQuery(std::string_view record) noexcept override {
        queries_.write(record.data(), static_cast<std::streamsize>(record.size()));
        return static_cast<bool>(queries_);
    }
    bool WriteSample(std::string_view record) noexcept override {
        samples_.write(record.data(), static_cast<std::streamsize>(record.size()));
        return static_cast<bool>(samples_);
    }
    bool Flush() noexcept override {
        queries_.flush();
        samples_.flush();
        return static_cast<bool>(queries_) && static_cast<bool>(samples_);
    }
    bool Close() {
        queries_.close();
        samples_.close();
        return static_cast<bool>(queries_) && static_cast<bool>(samples_);
    }
};

void WriteJson(const std::filesystem::path& path, const json::object& object) {
    std::ofstream stream(path, std::ios::binary);
    stream << json::serialize(object) << '\n';
    stream.close();
    if (!stream) throw std::runtime_error("Write JSON artifact failed: " + Utf8(path));
}

json::object Environment(const Arguments& arguments, const Config& config,
                         std::uint64_t load_ns, const BenchmarkReport* report) {
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    const bool memory_ok = GlobalMemoryStatusEx(&memory) != 0;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job{};
    const bool job_ok = QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation,
        &job, sizeof(job), nullptr) != 0;
    const bool job_limited = job_ok &&
        (job.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY);
    wchar_t root[MAX_PATH]{}, filesystem[MAX_PATH]{};
    const bool volume_ok = GetVolumePathNameW(arguments.output.c_str(), root, MAX_PATH) &&
        GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, filesystem, MAX_PATH);
    json::object output{
        {"schema_version", 1}, {"protocol_version", "EVAL-1.1"},
        {"purpose", config.benchmark.purpose}, {"run_id", config.benchmark.run_id},
        {"target", "contract-test"}, {"formal_engine_measurement", false},
        {"engine_options_applied", false},
        {"input_content_used", false}, {"input_hashes_verified", false},
        {"code_commit_at_configure", AGENT_IO_BUILD_COMMIT},
        {"code_dirty_at_configure", AGENT_IO_BUILD_DIRTY},
        {"compiler", "MSVC"}, {"compiler_version", _MSC_VER},
        {"build_config", AGENT_IO_BUILD_CONFIG},
        {"platform", "native Windows x64"},
        {"os_version", nullptr}, {"os_version_unavailable_reason", "Not probed by this runner"},
        {"logical_processors", system.dwNumberOfProcessors},
        {"physical_memory_bytes", memory_ok ? json::value(std::to_string(memory.ullTotalPhys)) :
            json::value(nullptr)},
        {"cpu_affinity", nullptr}, {"cpu_affinity_unavailable_reason", "Not pinned by runner"},
        {"pid", std::to_string(GetCurrentProcessId())},
        {"participating_processes", "one C++ benchmark process; target starts no child processes"},
        {"measurement_domain", "runner, logger, workers and target in current inherited Job"},
        {"job_limit_commit_bytes", job_limited ? json::value(std::to_string(job.JobMemoryLimit)) :
            json::value(nullptr)},
        {"job_limit_unavailable_reason", job_limited ? json::value(nullptr) :
            json::value("No current Job memory limit could be established")},
        {"external_job_metrics", arguments.job_metrics.empty() ? json::value(nullptr) :
            json::value(Utf8(std::filesystem::absolute(arguments.job_metrics)))},
        {"sample_interval_ms", std::to_string(config.benchmark.sample_interval.count())},
        {"sampling_policy", "logger samples at actual times; congestion can delay samples"},
        {"workers", config.benchmark.limits.workers},
        {"runtime_threads", config.benchmark.limits.workers + std::uint64_t{2}},
        {"thread_stack_accounting", "OS-managed stacks observed by external Job; not governor heaps"},
        {"io_backend", "none"}, {"direct_reads", false}, {"verified_alignment", nullptr},
        {"diskann", nullptr}, {"boost_json_version", "1.78.0"},
        {"system_file_cache_bytes", nullptr},
        {"system_file_cache_unavailable_reason", "Not observed"},
        {"log_directory", Utf8(arguments.output)},
        {"log_volume", volume_ok ? json::value(Utf8(std::filesystem::path(root))) :
            json::value(nullptr)},
        {"log_filesystem", volume_ok ? json::value(Utf8(std::filesystem::path(filesystem))) :
            json::value(nullptr)},
        {"log_policy", "bounded logger; binary UTF-8 JSONL; flush and close before counters"},
        {"query_selection", "(seed % query_count + ordinal % query_count) % query_count"},
        {"index_load_ns", std::to_string(load_ns)},
        {"index_load_kind", "contract-test target construction; no index loaded"},
        {"test_delay_ms", arguments.delay_ms}, {"test_error_every", arguments.error_every},
        {"cleanup_policy", "freeze publication at deadline; stop, join and quiesce target"},
        {"limitations", json::array{
            "Contract-test neighbors are synthetic; no retrieval or recall claim.",
            "Measurement purpose requires the B-side Engine target, which is not linked.",
            "Queue accounting is conservative, not an allocator or physical-memory proof.",
            "An uncooperative target requires an external Job process timeout."}}};
    if (report) {
        output["runtime_workspace_bytes"] = std::to_string(report->resources.workspace_bytes);
        output["log_bytes"] = std::to_string(report->query_log_bytes + report->sample_log_bytes);
        output["warmup_elapsed_ns"] = std::to_string(report->warmup_elapsed_ns);
        output["cleanup_ns"] = std::to_string(report->cleanup_ns);
    }
    return output;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--help") {
        std::puts("vector_bench --config run.json --output new-directory "
                  "[--target contract-test] [--job-metrics metrics.json]\n"
                  "contract-test accepts only purpose=contract_example. "
                  "The Engine target is not yet linked.\n"
                  "Test controls: --test-delay-ms N --test-error-every N (0..60000).");
        return 0;
    }
    bool output_created = false;
    std::filesystem::path output;
    try {
        auto arguments = ParseArguments(argc, argv);
        auto config = ReadConfig(arguments.config);
        if (arguments.target == L"engine") {
            std::fputs("Engine target is not linked. Use --target contract-test only "
                       "for producer verification; B must provide the real adapter.\n", stderr);
            return 3;
        }
        if (config.benchmark.purpose != "contract_example") {
            std::fputs("contract-test refuses purpose=measurement.\n", stderr);
            return 3;
        }
        arguments.output = std::filesystem::absolute(arguments.output).lexically_normal();
        output = arguments.output;
        std::error_code error;
        if (!std::filesystem::create_directory(output, error))
            throw ConfigError("Output must be a new directory with an existing parent");
        output_created = true;
        auto governor = Take(CreateResourceGovernor(config.memory));
        // Retained config, stream buffers and post-run JSON summaries. OS stacks
        // and the CRT remain visible to the external Job, not hidden as heaps.
        auto cli_memory = Take(governor->TryReserve({MemoryRegion::workspace, 4194304}));
        Files files(output);
        std::ofstream standard_log(output / "stdout.log", std::ios::binary);
        std::ofstream error_log(output / "stderr.log", std::ios::binary);
        if (!standard_log || !error_log) throw std::runtime_error("Open process logs failed");
        WriteJson(output / "effective-config.json", config.effective);
        const auto load_start = Clock::now();
        ContractTarget target(config, arguments);
        const auto load_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - load_start).count());
        WriteJson(output / "environment.json", Environment(arguments, config, load_ns, nullptr));
        const auto result = RunStaticBenchmark(config.benchmark, target, files, governor);
        if (const auto* issue = std::get_if<Error>(&result)) {
            error_log << issue->message << '\n';
            error_log.close();
            files.Close();
            std::fprintf(stderr, "Benchmark failed: %s\n", issue->message.c_str());
            return issue->code == ErrorCode::invalid_argument ? 2 : 4;
        }
        auto report = std::get<BenchmarkReport>(result);
        if (!files.Close()) {
            report.run_incomplete = true;
            report.exit_code = 5;
            report.diagnostic = "Streaming artifact close/flush failed";
        }
        WriteJson(output / "counters.json", Counters(config.benchmark, report));
        WriteJson(output / "summary.json", Summary(config.benchmark, report));
        WriteJson(output / "environment.json", Environment(arguments, config, load_ns, &report));
        const auto summary = json::serialize(Summary(config.benchmark, report));
        standard_log << summary << '\n';
        if (!report.diagnostic.empty()) error_log << report.diagnostic << '\n';
        standard_log.close();
        error_log.close();
        if (!standard_log || !error_log) return 5;
        std::printf("target=contract-test; formal_engine_measurement=false; "
            "scheduled=%llu; succeeded=%llu; failed=%llu; rejected=%llu; pending=%llu; exit=%d\n",
            static_cast<unsigned long long>(report.query.scheduled),
            static_cast<unsigned long long>(report.query.succeeded),
            static_cast<unsigned long long>(report.query.failed),
            static_cast<unsigned long long>(report.query.rejected),
            static_cast<unsigned long long>(report.query.pending), report.exit_code);
        return report.exit_code;
    } catch (const ConfigError& error) {
        std::fprintf(stderr, "Configuration error: %s\n", error.what());
        return 2;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "vector_bench failed: %s\n", error.what());
        return output_created ? 5 : 2;
    }
}
