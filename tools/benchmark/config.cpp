#include "config.hpp"

#include <boost/json/basic_parser_impl.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <set>

namespace agent_io::bench_cli {
namespace {
namespace json = boost::json;
constexpr std::uint64_t kU64Max = UINT64_MAX;

void Require(bool condition, const char* message) {
    if (!condition) throw ConfigError(message);
}

bool Listed(std::initializer_list<json::string_view> names, json::string_view key) {
    for (auto name : names)
        if (name == key) return true;
    return false;
}

// Optional fields are declared explicitly: an unknown key is still a mistake,
// but a run configuration may carry the evaluation sections that the Python
// consumer needs.
const json::object& Object(const json::value& value,
                          std::initializer_list<json::string_view> fields,
                          std::initializer_list<json::string_view> optional = {}) {
    Require(value.is_object(), "Expected JSON object");
    const auto& object = value.as_object();
    Require(object.size() >= fields.size() && object.size() <= fields.size() + optional.size(),
            "Unknown or missing configuration field");
    for (auto field : fields)
        Require(object.contains(field), "Unknown or missing configuration field");
    for (const auto& member : object)
        Require(Listed(fields, member.key()) || Listed(optional, member.key()),
                "Unknown configuration field");
    return object;
}

std::string Text(const json::value& value, std::size_t maximum = 32768) {
    Require(value.is_string() && !value.as_string().empty(), "Expected nonempty string");
    const auto& text = value.as_string();
    Require(text.size() <= maximum && text.find('\0') == json::string::npos,
            "String is too long or contains NUL");
    return {text.data(), text.size()};
}

std::uint64_t U64(const json::value& value, bool positive = false) {
    const auto text = Text(value, 20);
    Require(text == "0" || (text.front() >= '1' && text.front() <= '9'),
            "Expected canonical unsigned decimal string");
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    Require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
            (!positive || result), "Invalid uint64 string");
    return result;
}

std::uint32_t U32(const json::value& value, bool positive = false) {
    Require(value.is_int64() || value.is_uint64(), "Expected JSON integer, not boolean");
    const auto number = value.is_uint64() ? value.as_uint64() :
        value.as_int64() < 0 ? kU64Max : static_cast<std::uint64_t>(value.as_int64());
    Require(number <= UINT32_MAX && (!positive || number), "Invalid uint32 integer");
    return static_cast<std::uint32_t>(number);
}

double Number(const json::value& value) {
    Require(value.is_number(), "Expected finite JSON number");
    const auto number = value.to_number<double>();
    Require(std::isfinite(number), "Expected finite JSON number");
    return number;
}

std::chrono::milliseconds Milliseconds(const json::value& value, bool positive = false) {
    const auto count = U64(value, positive);
    Require(count <= kU64Max / 1000000 && count <= INT64_MAX, "Duration overflow");
    return std::chrono::milliseconds(static_cast<std::int64_t>(count));
}

MemoryBudget Memory(const json::value& value) {
    const auto& object = Object(value, {"total_bytes", "cache_bytes", "delta_bytes",
        "metadata_bytes", "workspace_bytes", "reserve_bytes"});
    MemoryBudget budget{U64(object.at("total_bytes"), true),
        U64(object.at("cache_bytes")), U64(object.at("delta_bytes"), true),
        U64(object.at("metadata_bytes"), true), U64(object.at("workspace_bytes"), true),
        U64(object.at("reserve_bytes"), true)};
    std::uint64_t sum = 0;
    for (auto part : {budget.cache_bytes, budget.delta_bytes, budget.metadata_bytes,
                      budget.workspace_bytes, budget.reserve_bytes}) {
        Require(part <= kU64Max - sum, "Memory partition overflow");
        sum += part;
    }
    Require(sum == budget.total_bytes, "Memory partitions do not sum to total");
    return budget;
}

void Checksum(const json::value& value, bool example) {
    if (value.is_null()) {
        Require(example, "Missing measurement checksum");
        return;
    }
    const auto text = Text(value, 64);
    Require(text.size() == 64 && std::all_of(text.begin(), text.end(), [](char item) {
        return (item >= '0' && item <= '9') || (item >= 'a' && item <= 'f');
    }), "SHA256 must have 64 lowercase hexadecimal characters");
}

// Boost.JSON's DOM accepts duplicate keys. A SAX pass rejects them, including
// escaped aliases, before the same mature parser constructs the DOM.
struct UniqueKeys {
    static constexpr std::size_t max_array_size = 4096;
    static constexpr std::size_t max_object_size = 4096;
    static constexpr std::size_t max_string_size = 32768;
    static constexpr std::size_t max_key_size = 32768;
    std::vector<std::set<std::string>> objects;
    std::string key;
    bool on_document_begin(json::error_code&) { return true; }
    bool on_document_end(json::error_code&) { return true; }
    bool on_object_begin(json::error_code&) { objects.emplace_back(); return true; }
    bool on_object_end(std::size_t, json::error_code&) { objects.pop_back(); return true; }
    bool on_array_begin(json::error_code&) { return true; }
    bool on_array_end(std::size_t, json::error_code&) { return true; }
    bool on_key_part(json::string_view text, std::size_t, json::error_code&) {
        key.append(text.data(), text.size()); return true;
    }
    bool on_key(json::string_view text, std::size_t, json::error_code& error) {
        key.append(text.data(), text.size());
        const bool unique = objects.back().insert(key).second;
        key.clear();
        if (!unique) error = json::error::syntax;
        return unique;
    }
    bool on_string_part(json::string_view, std::size_t, json::error_code&) { return true; }
    bool on_string(json::string_view, std::size_t, json::error_code&) { return true; }
    bool on_number_part(json::string_view, json::error_code&) { return true; }
    bool on_int64(std::int64_t, json::string_view, json::error_code&) { return true; }
    bool on_uint64(std::uint64_t, json::string_view, json::error_code&) { return true; }
    bool on_double(double value, json::string_view, json::error_code& error) {
        if (std::isfinite(value)) return true;
        error = json::error::syntax; return false;
    }
    bool on_bool(bool, json::error_code&) { return true; }
    bool on_null(json::error_code&) { return true; }
    bool on_comment_part(json::string_view, json::error_code&) { return true; }
    bool on_comment(json::string_view, json::error_code&) { return true; }
};

json::value ParseJson(std::string_view text) {
    json::basic_parser<UniqueKeys> parser(json::parse_options{});
    json::error_code error;
    const auto consumed = parser.write_some(false, text.data(), text.size(), error);
    Require(!error && consumed == text.size() && parser.done(),
            "Invalid JSON, duplicate key, nonfinite number or trailing content");
    return json::parse(json::string_view(text.data(), text.size()));
}

json::object Counts(const RequestCounts& counts) {
    return {{"scheduled", std::to_string(counts.scheduled)},
        {"succeeded", std::to_string(counts.succeeded)},
        {"failed", std::to_string(counts.failed)},
        {"rejected", std::to_string(counts.rejected)},
        {"pending", std::to_string(counts.pending)},
        {"window_completed", std::to_string(counts.window_completed)},
        {"window_succeeded", std::to_string(counts.window_succeeded)}};
}

json::object Header(const BenchmarkOptions& options) {
    return {{"schema_version", 1}, {"protocol_version", "EVAL-1.1"},
        {"purpose", options.purpose}, {"run_id", options.run_id}};
}

json::object Latency(const BenchmarkLatency& value) {
    json::array bins;
    for (auto count : value.bins) bins.emplace_back(std::to_string(count));
    return {{"count", std::to_string(value.count)},
        {"min_ns", value.count ? json::value(std::to_string(value.min_ns)) : json::value(nullptr)},
        {"max_ns", value.count ? json::value(std::to_string(value.max_ns)) : json::value(nullptr)},
        {"power_of_two_ns_bins", std::move(bins)}};
}
}  // namespace

std::string Utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

Config ReadConfig(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    Require(static_cast<bool>(input), "Cannot open configuration");
    std::string text(1048577, '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    const auto size = input.gcount();
    Require(size > 0 && size <= 1048576, "Configuration must fit in 1 MiB");
    text.resize(static_cast<std::size_t>(size));
    const auto document = ParseJson(text);
    const auto& root = Object(document, {"schema_version", "protocol_version", "purpose",
        "run_id", "dataset", "engine", "search", "workload", "measurement"},
        {"index", "trace", "answers", "unbudgeted_metadata_bytes"});
    Require(U32(root.at("schema_version")) == 1 &&
            Text(root.at("protocol_version")) == "EVAL-1.1", "Unsupported protocol");
    Config config;
    auto& options = config.benchmark;
    options.run_id = Text(root.at("run_id"), 128);
    options.purpose = Text(root.at("purpose"));
    Require(options.purpose == "measurement" || options.purpose == "contract_example",
            "Unsupported purpose");
    const auto& dataset = Object(root.at("dataset"), {"format", "vectors", "ids",
        "queries", "query_ids", "vector_count", "query_count", "dimension", "metric", "sha256"},
        {"updates"});
    Require(Text(dataset.at("format")) == "raw-f32le-v1" &&
            Text(dataset.at("metric")) == "squared_l2", "Unsupported dataset format or metric");
    config.vector_count = U64(dataset.at("vector_count"), true);
    options.query_count = U64(dataset.at("query_count"), true);
    config.dimension = U32(dataset.at("dimension"), true);
    const auto& hashes = Object(dataset.at("sha256"), {"vectors", "ids", "queries", "query_ids"},
                                {"updates"});
    for (auto name : {"vectors", "ids", "queries", "query_ids"}) {
        Text(dataset.at(name));
        Checksum(hashes.at(name), options.purpose == "contract_example");
    }
    const auto& engine = Object(root.at("engine"), {"directory", "io_backend", "direct_reads",
        "cache_policy", "block_bytes", "memory", "rebuild_memory", "resources",
        "read_scheduling", "wal", "max_queries", "max_pending_writes", "max_inflight_reads",
        "max_prefetch_reads", "max_delta_segments", "group_commit_window_ms"});
    Text(engine.at("directory"));
    const auto backend = Text(engine.at("io_backend"));
    Require(backend == "synchronous" || backend == "windows_iocp", "Invalid I/O backend");
    const auto cache = Text(engine.at("cache_policy"));
    Require(cache == "disabled" || cache == "clock", "Invalid cache policy");
    Require(engine.at("direct_reads").is_bool(), "Expected direct_reads boolean");
    U32(engine.at("block_bytes"), true);
    config.memory = Memory(engine.at("memory"));
    const auto& resources = Object(engine.at("resources"), {"max_disk_segments",
        "max_retained_generations", "max_retained_delta_vectors",
        "background_workspace_bytes", "min_free_disk_bytes"});
    U32(resources.at("max_disk_segments"), true);
    Require(U32(resources.at("max_retained_generations"), true) >= 2,
            "At least two retained generations required");
    const auto retained = U32(resources.at("max_retained_delta_vectors"), true);
    const auto background = U64(resources.at("background_workspace_bytes"), true);
    U64(resources.at("min_free_disk_bytes"));
    const auto delta_record = std::uint64_t{config.dimension} * 4 + 32;
    Require(delta_record <= config.memory.delta_bytes / retained, "Retained delta exceeds budget");
    if (!engine.at("rebuild_memory").is_null()) {
        const auto rebuild = Memory(engine.at("rebuild_memory"));
        Require(rebuild.total_bytes == config.memory.total_bytes &&
                background < rebuild.workspace_bytes &&
                delta_record <= rebuild.delta_bytes / retained, "Invalid rebuild budget");
    }
    U32(engine.at("max_queries"), true);
    U32(engine.at("max_pending_writes"), true);
    const auto inflight = U32(engine.at("max_inflight_reads"), true);
    const auto prefetch = U32(engine.at("max_prefetch_reads"));
    U32(engine.at("max_delta_segments"), true);
    Require(U64(engine.at("group_commit_window_ms")) <= INT64_MAX, "Group commit overflow");
    const auto& scheduling = Object(engine.at("read_scheduling"), {"demand_reserve",
        "background_reserve", "max_background_reads", "demand_burst_before_background"});
    const auto demand = U32(scheduling.at("demand_reserve"), true);
    const auto background_reserve = U32(scheduling.at("background_reserve"), true);
    const auto background_max = U32(scheduling.at("max_background_reads"), true);
    U32(scheduling.at("demand_burst_before_background"), true);
    Require(demand < inflight && background_reserve <= background_max &&
            background_max <= inflight - demand &&
            prefetch <= inflight - demand - background_reserve, "Invalid I/O reservations");
    const auto& wal = Object(engine.at("wal"), {"rotation_bytes", "max_frame_bytes"});
    const auto frame = U64(wal.at("max_frame_bytes"), true);
    Require(frame <= UINT32_MAX && frame >= ((64 + config.dimension * std::uint64_t{4} +
            4 + 7) / 8 * 8) && U64(wal.at("rotation_bytes"), true) >= frame + 64,
            "Invalid WAL frame or rotation bound");
    const auto& search = Object(root.at("search"), {"k", "candidate_limit",
        "prefetch_blocks", "timeout_ms"});
    options.search.k = U32(search.at("k"), true);
    options.search.candidate_limit = U32(search.at("candidate_limit"), true);
    options.search.prefetch_blocks = U32(search.at("prefetch_blocks"));
    Require(options.search.prefetch_blocks <= prefetch, "Query prefetch exceeds cap");
    options.search.timeout = Milliseconds(search.at("timeout_ms"), true);
    const auto& workload = Object(root.at("workload"), {"mode", "target_ops_per_second",
        "workers", "client_queue_capacity", "client_queue_bytes",
        "query_fraction", "insert_fraction", "upsert_fraction", "seed"});
    Require(Text(workload.at("mode")) == "open_loop", "Only open_loop is implemented");
    options.target_ops_per_second = Number(workload.at("target_ops_per_second"));
    options.limits.workers = U32(workload.at("workers"), true);
    options.limits.client_queue_capacity = U32(workload.at("client_queue_capacity"), true);
    options.limits.client_queue_bytes = U64(workload.at("client_queue_bytes"), true);
    const auto query_fraction = Number(workload.at("query_fraction"));
    const auto insert_fraction = Number(workload.at("insert_fraction"));
    const auto upsert_fraction = Number(workload.at("upsert_fraction"));
    Require(query_fraction > 0 && insert_fraction >= 0 && upsert_fraction >= 0 &&
            std::abs(query_fraction + insert_fraction + upsert_fraction - 1.0) <= 1e-9,
            "Invalid query, insert or upsert fractions");
    config.mixed = insert_fraction > 0 || upsert_fraction > 0;
    options.seed = U64(workload.at("seed"));
    const auto& measurement = Object(root.at("measurement"), {"warmup_ms", "duration_ms",
        "drain_timeout_ms", "sample_interval_ms", "result_queue_capacity",
        "result_queue_bytes", "max_record_bytes"});
    options.warmup = Milliseconds(measurement.at("warmup_ms"));
    options.duration = Milliseconds(measurement.at("duration_ms"), true);
    options.drain_timeout = Milliseconds(measurement.at("drain_timeout_ms"), true);
    options.sample_interval = Milliseconds(measurement.at("sample_interval_ms"), true);
    options.limits.result_queue_capacity = U32(measurement.at("result_queue_capacity"), true);
    options.limits.result_queue_bytes = U64(measurement.at("result_queue_bytes"), true);
    options.limits.max_record_bytes = U32(measurement.at("max_record_bytes"), true);
    Require(options.limits.client_queue_bytes >= (std::max)(
        std::uint64_t{options.limits.max_record_bytes}, std::uint64_t{config.dimension} * 4 + 1024),
        "Client byte cap below query bound");
    const auto plan = PlanStaticBenchmark(options);
    if (const auto* issue = std::get_if<Error>(&plan)) throw ConfigError(issue->message);
    // Engine options are parsed here so the real target can open the index with
    // exactly the validated numbers, not a second interpretation of the JSON.
    {
        auto& target = config.engine;
        target.memory = config.memory;
        if (!engine.at("rebuild_memory").is_null())
            target.rebuild_memory = Memory(engine.at("rebuild_memory"));
        target.io_backend = backend == "synchronous" ? IoBackend::synchronous :
            IoBackend::windows_iocp;
        target.cache_policy = cache == "disabled" ? CachePolicy::disabled : CachePolicy::clock;
        target.direct_reads = engine.at("direct_reads").as_bool();
        target.block_bytes = U32(engine.at("block_bytes"), true);
        target.resources.max_disk_segments = U32(resources.at("max_disk_segments"), true);
        target.resources.max_retained_generations =
            U32(resources.at("max_retained_generations"), true);
        target.resources.max_retained_delta_vectors = retained;
        target.resources.background_workspace_bytes = background;
        target.resources.min_free_disk_bytes = U64(resources.at("min_free_disk_bytes"));
        target.read_scheduling.demand_reserve = demand;
        target.read_scheduling.background_reserve = background_reserve;
        target.read_scheduling.max_background_reads = background_max;
        target.read_scheduling.demand_burst_before_background =
            U32(scheduling.at("demand_burst_before_background"), true);
        target.wal.rotation_bytes = U64(wal.at("rotation_bytes"), true);
        target.wal.max_frame_bytes = static_cast<std::uint32_t>(frame);
        target.max_queries = U32(engine.at("max_queries"), true);
        target.max_pending_writes = U32(engine.at("max_pending_writes"), true);
        target.max_inflight_reads = inflight;
        target.max_prefetch_reads = prefetch;
        target.max_delta_segments = U32(engine.at("max_delta_segments"), true);
        target.group_commit_window = std::chrono::milliseconds(
            static_cast<std::int64_t>(U64(engine.at("group_commit_window_ms"))));
    }

    config.vectors_sha256 = hashes.at("vectors").is_null() ? std::string{} :
        Text(hashes.at("vectors"), 64);
    config.ids_sha256 = hashes.at("ids").is_null() ? std::string{} : Text(hashes.at("ids"), 64);
    config.queries_sha256 = hashes.at("queries").is_null() ? std::string{} :
        Text(hashes.at("queries"), 64);
    config.query_ids_sha256 = hashes.at("query_ids").is_null() ? std::string{} :
        Text(hashes.at("query_ids"), 64);

    if (const auto* index_section = root.if_contains("index")) {
        const auto& index = Object(*index_section, {"directory", "build_id",
                                                   "manifest_generation"}, {});
        Text(index.at("build_id"), 128);
        U64(index.at("manifest_generation"), true);
        Require(Text(index.at("directory")) == Text(engine.at("directory")),
                "index.directory must equal engine.directory");
        config.complete_evaluation_config = true;
    }
    if (const auto* trace_section = root.if_contains("trace"))
        Require(trace_section->is_object(), "trace must be an object");
    if (const auto* answers_section = root.if_contains("answers")) {
        const auto& answers = Object(*answers_section, {"file", "method"}, {});
        Require(Text(answers.at("method")) == "exact_scan_float64",
                "Unsupported independent answer method");
        Text(answers.at("file"));
        config.complete_evaluation_config = config.complete_evaluation_config &&
            root.if_contains("trace") != nullptr;
    }

    // effective-config.json must keep the authored run-relative paths: EVAL-1.1
    // forbids absolute paths and "..", and the published artifact is validated
    // against the run directory it lands in. Resolution happens only for the
    // internal handles below.
    config.effective = root;
    const auto base = std::filesystem::absolute(path).parent_path();
    auto resolve = [&](const json::value& value) {
        const auto source = Text(value);
        const std::u8string encoded(reinterpret_cast<const char8_t*>(source.data()), source.size());
        return Utf8(std::filesystem::absolute(base /
            std::filesystem::path(encoded)).lexically_normal());
    };
    auto to_path = [](const std::string& text) {
        return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()),
                                                   text.size()));
    };
    config.directory = to_path(resolve(engine.at("directory")));
    config.vectors = to_path(resolve(dataset.at("vectors")));
    config.ids = to_path(resolve(dataset.at("ids")));
    config.queries = to_path(resolve(dataset.at("queries")));
    config.query_ids = to_path(resolve(dataset.at("query_ids")));
    if (config.mixed) {
        Require(root.contains("index") && root.contains("trace") && root.contains("answers") &&
                    dataset.contains("updates") && hashes.contains("updates"),
                "Mixed workloads require index, trace, answers and update payload");
        const auto& trace = Object(root.at("trace"), {"file", "sha256", "operations",
            "queries", "seed"}, {"update_distribution", "snapshot_policy"});
        Checksum(trace.at("sha256"), false);
        Checksum(hashes.at("updates"), false);
        config.trace = to_path(resolve(trace.at("file")));
        config.trace_sha256 = Text(trace.at("sha256"), 64);
        config.updates = to_path(resolve(dataset.at("updates")));
        config.updates_sha256 = Text(hashes.at("updates"), 64);
        static_cast<void>(U64(trace.at("seed")));
    }
    return config;
}

std::vector<BenchmarkEvent> ReadTrace(const Config& config) {
    std::ifstream input(config.trace, std::ios::binary);
    Require(static_cast<bool>(input), "Cannot open the fixed trace");
    std::vector<BenchmarkEvent> events;
    std::set<std::string> ids;
    std::array<std::uint64_t, 3> counts{};
    const auto& spec = config.effective.at("trace").as_object();
    const auto writes = U64(spec.at("operations"), true);
    const auto queries = U64(spec.at("queries"), true);
    Require(queries <= UINT32_MAX && writes <= UINT32_MAX - queries,
            "Trace length exceeds its supported range");
    const auto total = writes + queries;
    Require(total <= config.memory.workspace_bytes / 512, "Trace exceeds workspace budget");
    events.reserve(static_cast<std::size_t>(total));
    char line[4097];
    while (input.getline(line, sizeof(line))) {
        Require(events.size() < total, "Trace contains more events than declared");
        const auto length = static_cast<std::size_t>(input.gcount()) - (input.eof() ? 0 : 1);
        const auto document = ParseJson(std::string_view(line, length));
        const auto& row = Object(document, {"schema_version", "protocol_version", "purpose",
            "run_id", "arrival_index", "kind", "operation_id"},
            {"operation", "id", "payload_row", "query_id", "query_row"});
        Require(U32(row.at("schema_version")) == 1 &&
                    Text(row.at("protocol_version")) == "EVAL-1.1" &&
                    Text(row.at("purpose")) == config.benchmark.purpose &&
                    Text(row.at("run_id")) == config.benchmark.run_id &&
                    U32(row.at("arrival_index")) == events.size(),
                "Invalid trace header or arrival order");
        auto id = Text(row.at("operation_id"), 128);
        Require(ids.insert(id).second, "Duplicate trace operation ID");
        const auto kind = Text(row.at("kind"));
        if (kind == "query") {
            Require(row.size() == 9 && row.contains("query_id") && row.contains("query_row"),
                    "Invalid query trace row");
            const auto position = U64(row.at("query_row"));
            Require(position < config.benchmark.query_count, "Query trace row is outside dataset");
            events.push_back({BenchmarkOperation::query, std::move(id), position, U64(row.at("query_id"))});
        } else {
            Require(kind == "write" && row.size() == 10 && row.contains("operation") &&
                        row.contains("id") && row.contains("payload_row"), "Invalid write trace row");
            const auto operation = Text(row.at("operation"));
            Require(operation == "insert" || operation == "upsert", "Invalid trace write operation");
            events.push_back({operation == "insert" ? BenchmarkOperation::insert : BenchmarkOperation::upsert,
                              std::move(id), U64(row.at("payload_row")), U64(row.at("id"))});
        }
        ++counts[static_cast<std::size_t>(events.back().operation)];
    }
    Require(input.eof() && events.size() == total && counts[0] == queries,
            "Trace is truncated, oversized or has incorrect counts");
    const auto& workload = config.effective.at("workload").as_object();
    const char* fractions[] = {"query_fraction", "insert_fraction", "upsert_fraction"};
    for (std::size_t i = 0; i < counts.size(); ++i)
        Require(std::abs(Number(workload.at(fractions[i])) -
                        static_cast<double>(counts[i]) / static_cast<double>(total)) <= 1e-9,
                "Workload fractions disagree with the trace");
    return events;
}

boost::json::object Counters(const BenchmarkOptions& options, const BenchmarkReport& report) {
    auto output = Header(options);
    output["duration_ns"] = std::to_string(report.duration_ns);
    output["drain_ns"] = std::to_string(report.drain_ns);
    output["run_incomplete"] = report.run_incomplete;
    output["requires_review"] = report.requires_review;
    output["measurement_start_sequence"] = std::to_string(report.start_stats.published_sequence);
    output["measurement_end_sequence"] = std::to_string(report.end_stats.published_sequence);
    output["by_type"] = json::object{{"query", Counts(report.query)},
        {"insert", Counts(report.insert)}, {"upsert", Counts(report.upsert)}};
    const auto& delta = report.counter_delta;
    output["engine_counters"] = json::object{
        {"io_reads_submitted", std::to_string(delta.io_reads_submitted)},
        {"io_read_bytes_completed", std::to_string(delta.io_read_bytes_completed)},
        {"io_read_errors", std::to_string(delta.io_read_errors)},
        {"prefetch_submitted", std::to_string(delta.prefetch_submitted)},
        {"prefetch_consumed", std::to_string(delta.prefetch_consumed)},
        {"wal_bytes_written", std::to_string(delta.wal_bytes_written)},
        {"checkpoint_bytes_written", std::to_string(delta.checkpoint_bytes_written)},
        {"merge_bytes_written", std::to_string(delta.merge_bytes_written)}};
    return output;
}

boost::json::object Summary(const BenchmarkOptions& options, const BenchmarkReport& report) {
    auto output = Header(options);
    output["exit_code"] = report.exit_code;
    output["diagnostic"] = report.diagnostic.empty() ? json::value(nullptr) :
        json::value(report.diagnostic);
    output["window_completed_qps"] = report.query.window_succeeded * 1e9 /
        static_cast<double>(report.duration_ns);
    output["drained_qps"] = report.query.succeeded * 1e9 /
        static_cast<double>(report.duration_ns + report.drain_ns);
    output["latency_histogram_encoding"] = "bin 0: 0 ns; bin i: [2^(i-1), 2^i-1] ns";
    json::object latencies;
    const char* statuses[] = {"ok", "error", "rejected"};
    for (std::size_t i = 0; i < 3; ++i)
        latencies[statuses[i]] = json::object{{"end_to_end", Latency(report.end_to_end[i])},
            {"queue", Latency(report.queue[i])}, {"service", Latency(report.service[i])}};
    output["latencies"] = std::move(latencies);
    if (report.insert.scheduled || report.upsert.scheduled) {
        json::object writes;
        const RequestCounts kinds[] = {report.insert, report.upsert};
        const char* names[] = {"insert", "upsert"};
        for (std::size_t i = 0; i < std::size(kinds); ++i)
            writes[names[i]] = json::object{
                {"window_confirmed_per_second", kinds[i].window_succeeded * 1e9 /
                                               static_cast<double>(report.duration_ns)},
                {"drained_confirmed_per_second", kinds[i].succeeded * 1e9 /
                    static_cast<double>(report.duration_ns + report.drain_ns)},
                {"end_to_end", Latency(report.write_end_to_end[i])},
                {"queue", Latency(report.write_queue[i])},
                {"service", Latency(report.write_service[i])}};
        output["writes"] = std::move(writes);
        output["write_log_bytes"] = std::to_string(report.write_log_bytes);
    }
    output["read_path"] = json::object{
        {"cache_hits", std::to_string(report.counter_delta.cache_hits)},
        {"cache_misses", std::to_string(report.counter_delta.cache_misses)},
        {"prefetch_ready_at_demand", std::to_string(report.counter_delta.prefetch_ready_at_demand)},
        {"cache_resident_bytes", std::to_string(report.end_stats.cache_resident_bytes)},
        {"engine_accounted_bytes", std::to_string(report.end_stats.memory_accounted_bytes)},
        {"peak_inflight_reads", report.end_stats.peak_inflight_reads},
        {"peak_prefetch_reads", report.end_stats.peak_prefetch_reads}};
    constexpr const char* error_codes[] = {
        "invalid_argument", "not_found", "already_exists", "resource_exhausted",
        "deadline_exceeded", "search_incomplete", "io_error", "corruption",
        "unsupported", "closed", "recovery_required"};
    static_assert(static_cast<int>(ErrorCode::recovery_required) + 1 == std::size(error_codes));
    json::object errors;
    for (std::size_t i = 0; i < std::size(error_codes); ++i)
        errors[error_codes[i]] = std::to_string(report.errors_by_code[i]);
    output["errors_by_code"] = std::move(errors);
    output["api_timeouts"] = json::object{
        {"end_to_end", Latency(report.timeout_end_to_end)},
        {"queue", Latency(report.timeout_queue)}, {"service", Latency(report.timeout_service)}};
    output["query_log_bytes"] = std::to_string(report.query_log_bytes);
    output["sample_log_bytes"] = std::to_string(report.sample_log_bytes);
    output["cleanup_ns"] = std::to_string(report.cleanup_ns);
    output["warmup_elapsed_ns"] = std::to_string(report.warmup_elapsed_ns);
    output["warmup_query"] = Counts(report.warmup_query);
    output["resources"] = json::object{{"client_slots", report.resources.client_slots},
        {"result_slots", report.resources.result_slots},
        {"workspace_bytes", std::to_string(report.resources.workspace_bytes)}};
    return output;
}

}  // namespace agent_io::bench_cli
