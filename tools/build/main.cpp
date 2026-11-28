// vector_build: offline first build. Reads its configuration with the bounded
// JSON reader from the index library so the CLI adds no third-party dependency.

#include "agent_io/index.hpp"

#include "../../src/index/builder.hpp"
#include "../../src/index/digest.hpp"
#include "../../src/index/json.hpp"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

using namespace agent_io;
using index_json::Value;

struct ConfigError {
    std::string message;
};

[[noreturn]] void Fail(std::string message) { throw ConfigError{std::move(message)}; }

void RequireKeys(const Value& object, std::initializer_list<std::string_view> keys,
                 std::string_view context) {
    if (!object.IsObject()) Fail("Expected a JSON object for " + std::string(context));
    if (object.MemberCount() != keys.size())
        Fail("Unknown or missing field in " + std::string(context));
    for (const auto key : keys)
        if (object.Find(key) == nullptr)
            Fail("Missing field " + std::string(key) + " in " + std::string(context));
}

std::string Text(const Value& value, std::string_view name, std::size_t maximum = 4096,
                 bool allow_empty = false) {
    if (!value.IsString()) Fail(std::string(name) + " must be a string");
    const auto& text = value.AsString();
    if ((!allow_empty && text.empty()) || text.size() > maximum)
        Fail(std::string(name) + " length is out of range");
    return text;
}

std::uint64_t U64(const Value& value, std::string_view name, bool positive = false) {
    if (value.IsString()) {
        const auto& text = value.AsString();
        if (text.empty() || (text.size() > 1 && text.front() == '0'))
            Fail(std::string(name) + " must be a canonical decimal string");
        std::uint64_t result = 0;
        for (const char digit : text) {
            if (digit < '0' || digit > '9') Fail(std::string(name) + " is not decimal");
            const auto part = static_cast<std::uint64_t>(digit - '0');
            if (result > (UINT64_MAX - part) / 10) Fail(std::string(name) + " overflows");
            result = result * 10 + part;
        }
        if (positive && result == 0) Fail(std::string(name) + " must be positive");
        return result;
    }
    if (!value.IsInteger() || value.Negative()) Fail(std::string(name) + " must be an integer");
    if (positive && value.Magnitude() == 0) Fail(std::string(name) + " must be positive");
    return value.Magnitude();
}

std::uint32_t U32(const Value& value, std::string_view name, bool positive = false) {
    const auto number = U64(value, name, positive);
    if (number > 0xffffffffull) Fail(std::string(name) + " exceeds uint32");
    return static_cast<std::uint32_t>(number);
}

bool Boolean(const Value& value, std::string_view name) {
    if (!value.IsBoolean()) Fail(std::string(name) + " must be a boolean");
    return value.AsBool();
}

std::filesystem::path Path(const Value& value, std::string_view name) {
    const auto text = Text(value, name, 32768);
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()),
                                               text.size()));
}

struct BuildConfig {
    std::string purpose;
    std::string build_id;
    std::filesystem::path vectors;
    std::filesystem::path ids;
    std::string vectors_sha256;
    std::string ids_sha256;
    std::uint64_t vector_count = 0;
    std::uint32_t dimension = 0;
    std::uint32_t max_degree = 0;
    std::uint32_t build_beam = 0;
    std::string native_parameters;
    std::string native_version;
    std::uint64_t memory_limit_bytes = 0;
    std::uint64_t min_free_disk_bytes = 0;
    std::uint64_t max_peak_commit_bytes = 0;
    std::filesystem::path staging;
    std::filesystem::path output;
    std::uint64_t checkpoint_sequence = 0;
    std::uint64_t generation = 0;
    bool publish_current = false;
};

std::string ReadFile(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) Fail("Cannot stat configuration: " + path.string());
    if (size == 0 || size > (1u << 20)) Fail("Configuration must be between 1 byte and 1 MiB");
    std::ifstream input(path, std::ios::binary);
    if (!input) Fail("Cannot open configuration: " + path.string());
    std::string text(static_cast<std::size_t>(size), '\0');
    input.read(text.data(), static_cast<std::streamsize>(size));
    if (!input) Fail("Cannot read configuration: " + path.string());
    return text;
}

std::string OptionalChecksum(const Value& value, std::string_view name, bool example) {
    if (value.IsNull()) {
        if (!example) Fail(std::string(name) + " is required for a measurement build");
        return {};
    }
    const auto text = Text(value, name, 64);
    if (text.size() != 64) Fail(std::string(name) + " must have 64 characters");
    for (const char digit : text)
        if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f')))
            Fail(std::string(name) + " must be lowercase hexadecimal");
    return text;
}

BuildConfig Parse(const std::string_view text, const std::filesystem::path& base) {
    const auto parsed = index_json::Parse(text);
    if (!parsed.ok) Fail("Invalid JSON: " + parsed.error);
    const auto& root = parsed.value;
    RequireKeys(root, {"schema_version", "protocol_version", "purpose", "build_id", "dataset",
                       "native", "limits", "staging", "output", "notes"}, "build config");
    if (U32(*root.Find("schema_version"), "schema_version") != 1)
        Fail("Unsupported schema_version");
    if (Text(*root.Find("protocol_version"), "protocol_version") != "EVAL-1.1")
        Fail("Unsupported protocol_version");
    BuildConfig config;
    config.purpose = Text(*root.Find("purpose"), "purpose");
    if (config.purpose != "measurement" && config.purpose != "contract_example")
        Fail("Unsupported purpose");
    const bool example = config.purpose == "contract_example";
    config.build_id = Text(*root.Find("build_id"), "build_id", 128);

    const auto& dataset = *root.Find("dataset");
    RequireKeys(dataset, {"format", "vectors", "ids", "vector_count", "dimension", "metric",
                          "sha256"}, "dataset");
    if (Text(*dataset.Find("format"), "dataset.format") != "raw-f32le-v1")
        Fail("Unsupported dataset.format");
    if (Text(*dataset.Find("metric"), "dataset.metric") != "squared_l2")
        Fail("Unsupported dataset.metric");
    config.vectors = base / Path(*dataset.Find("vectors"), "dataset.vectors");
    config.ids = base / Path(*dataset.Find("ids"), "dataset.ids");
    config.vector_count = U64(*dataset.Find("vector_count"), "dataset.vector_count", true);
    config.dimension = U32(*dataset.Find("dimension"), "dataset.dimension", true);
    const auto& hashes = *dataset.Find("sha256");
    RequireKeys(hashes, {"vectors", "ids"}, "dataset.sha256");
    config.vectors_sha256 = OptionalChecksum(*hashes.Find("vectors"), "sha256.vectors", example);
    config.ids_sha256 = OptionalChecksum(*hashes.Find("ids"), "sha256.ids", example);

    const auto& native = *root.Find("native");
    RequireKeys(native, {"dependency", "version", "native_version", "max_degree", "build_beam",
                         "pq_bytes", "parameters"}, "native");
    const auto dependency = Text(*native.Find("dependency"), "native.dependency", 64);
    const auto dependency_version = Text(*native.Find("version"), "native.version", 64);
    config.native_version = Text(*native.Find("native_version"), "native.native_version", 64);
    // Both fields name the same base library. Allowing them to disagree would
    // publish a manifest claiming a version no artifact was built against.
    if (config.native_version != dependency_version)
        Fail("native.version and native.native_version disagree about " + dependency);
    const auto degree = U32(*native.Find("max_degree"), "native.max_degree", true);
    const auto beam = U32(*native.Find("build_beam"), "native.build_beam", true);
    if (degree > 512 || beam > degree) Fail("native.max_degree/build_beam are out of range");
    config.max_degree = degree;
    config.build_beam = beam;
    config.native_parameters = Text(*native.Find("parameters"), "native.parameters", 512);

    const auto& limits = *root.Find("limits");
    RequireKeys(limits, {"memory_limit_bytes", "min_free_disk_bytes", "job_object",
                         "measurement_scope", "max_peak_commit_bytes"}, "limits");
    config.memory_limit_bytes = U64(*limits.Find("memory_limit_bytes"), "limits.memory_limit_bytes",
                                    true);
    config.min_free_disk_bytes = U64(*limits.Find("min_free_disk_bytes"),
                                     "limits.min_free_disk_bytes");
    if (!Boolean(*limits.Find("job_object"), "limits.job_object"))
        Fail("limits.job_object must be true");
    if (Text(*limits.Find("measurement_scope"), "limits.measurement_scope") !=
        "build_process_tree")
        Fail("limits.measurement_scope must be build_process_tree");
    config.max_peak_commit_bytes = U64(*limits.Find("max_peak_commit_bytes"),
                                       "limits.max_peak_commit_bytes", true);
    if (config.max_peak_commit_bytes > config.memory_limit_bytes)
        Fail("max_peak_commit_bytes exceeds memory_limit_bytes");

    const auto& staging = *root.Find("staging");
    RequireKeys(staging, {"directory", "same_volume", "reuse_allowed"}, "staging");
    config.staging = base / Path(*staging.Find("directory"), "staging.directory");
    if (!Boolean(*staging.Find("same_volume"), "staging.same_volume"))
        Fail("staging.same_volume must be true");
    if (Boolean(*staging.Find("reuse_allowed"), "staging.reuse_allowed"))
        Fail("staging.reuse_allowed must be false");

    const auto& output = *root.Find("output");
    RequireKeys(output, {"directory", "checkpoint_sequence", "generation", "publish_current"},
                "output");
    config.output = base / Path(*output.Find("directory"), "output.directory");
    config.checkpoint_sequence = U64(*output.Find("checkpoint_sequence"),
                                     "output.checkpoint_sequence");
    config.generation = U64(*output.Find("generation"), "output.generation", true);
    config.publish_current = Boolean(*output.Find("publish_current"), "output.publish_current");
    if (config.checkpoint_sequence != 0) Fail("A first build must publish checkpoint 0");
    if (config.generation != 1) Fail("A first build must publish generation 1");
    if (!config.publish_current) Fail("A first build must publish CURRENT");

    const auto& notes = *root.Find("notes");
    if (!notes.IsArray() || notes.Size() == 0) Fail("notes must be a non-empty array");
    for (std::size_t index = 0; index < notes.Size(); ++index)
        Text(notes.At(index), "notes", 512);
    return config;
}

std::uint64_t JobCommitLimit() {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    if (!QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &info,
                                   sizeof(info), nullptr))
        return 0;
    if ((info.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY) == 0) return 0;
    return static_cast<std::uint64_t>(info.JobMemoryLimit);
}

void WriteReport(const std::filesystem::path& path, const BuildConfig& config,
                 const BuildOutcome& outcome, const std::string& adapter_name) {
    Value report = Value::Object();
    report.Insert("build_id", Value::String(config.build_id));
    report.Insert("purpose", Value::String(config.purpose));
    report.Insert("published", Value::Boolean(outcome.published));
    report.Insert("node_count", Value::String(std::to_string(outcome.artifacts.node_count)));
    report.Insert("file_bytes", Value::String(std::to_string(outcome.artifacts.file_bytes)));
    report.Insert("staged_bytes", Value::String(std::to_string(outcome.staged_bytes)));
    report.Insert("peak_commit_bytes", Value::String(std::to_string(outcome.peak_commit_bytes)));
    report.Insert("native_version", Value::String(outcome.native_version));
    report.Insert("graph_sha256", Value::String(FormatDigest(outcome.artifacts.content_sha256)));
    report.Insert("versions_sha256",
                  Value::String(FormatDigest(outcome.artifacts.version_directory_sha256)));
    report.Insert("segment_file", Value::String(outcome.artifacts.segment_file.string()));
    report.Insert("manifest_file", Value::String(outcome.artifacts.manifest_file.string()));
    report.Insert("adapter", Value::String(adapter_name));
    const auto text = index_json::Write(report);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text << '\n';
    if (!output) Fail("Cannot write the build report");
}

int Run(int argc, wchar_t** argv) {
    std::filesystem::path config_path, report_path;
    bool config_seen = false, report_seen = false;
    for (int index = 1; index < argc; ++index) {
        const std::wstring name(argv[index]);
        if (index + 1 == argc) Fail("Missing argument value");
        const auto* value = argv[++index];
        if (name == L"--config") {
            if (config_seen) Fail("Repeated --config");
            config_seen = true;
            config_path = value;
        } else if (name == L"--report") {
            if (report_seen) Fail("Repeated --report");
            report_seen = true;
            report_path = value;
        } else {
            Fail("Unknown command-line option");
        }
    }
    if (!config_seen) Fail("Usage: vector_build --config build.json [--report report.json]");
    config_path = std::filesystem::absolute(config_path).lexically_normal();
    const auto config = Parse(ReadFile(config_path), config_path.parent_path());

    const auto job_limit = JobCommitLimit();
    if (job_limit != 0 && job_limit < config.memory_limit_bytes)
        Fail("Current Job commit limit is smaller than limits.memory_limit_bytes");

    FirstBuildRequest request;
    request.vectors_path = config.vectors;
    request.ids_path = config.ids;
    request.vectors_sha256 = config.vectors_sha256;
    request.ids_sha256 = config.ids_sha256;
    request.dimension = config.dimension;
    request.max_degree = config.max_degree;
    request.build_beam = config.build_beam;
    request.vector_count = config.vector_count;
    request.memory_limit_bytes = config.memory_limit_bytes;
    request.min_free_disk_bytes = config.min_free_disk_bytes;
    request.staging_directory = config.staging;
    request.output_directory = config.output;
    request.build_id = config.build_id;
    request.native_parameters = config.native_parameters;
    request.native_version = config.native_version;
    request.checkpoint_sequence = config.checkpoint_sequence;
    request.generation = config.generation;
    request.publish_current = config.publish_current;

    auto outcome = RunFirstBuild(request);
    if (const auto* error = std::get_if<Error>(&outcome)) {
        std::fprintf(stderr, "vector_build failed: %s (code %d, system %d); published=false\n",
                     error->message.c_str(), static_cast<int>(error->code),
                     error->system_error);
        return error->code == ErrorCode::invalid_argument ? 2 : 4;
    }
    const auto& built = std::get<BuildOutcome>(outcome);
    if (built.peak_commit_bytes > config.max_peak_commit_bytes) {
        std::fprintf(stderr,
                     "vector_build exceeded limits.max_peak_commit_bytes (%llu > %llu); "
                     "published=%s\n",
                     static_cast<unsigned long long>(built.peak_commit_bytes),
                     static_cast<unsigned long long>(config.max_peak_commit_bytes),
                     built.published ? "true" : "false");
        if (report_seen) WriteReport(report_path, config, built, request.adapter_name);
        return 4;
    }
    if (report_seen) WriteReport(report_path, config, built, request.adapter_name);
    std::printf("published=%s nodes=%llu bytes=%llu peak_commit=%llu adapter=%s\n",
                built.published ? "true" : "false",
                static_cast<unsigned long long>(built.artifacts.node_count),
                static_cast<unsigned long long>(built.artifacts.file_bytes),
                static_cast<unsigned long long>(built.peak_commit_bytes),
                request.adapter_name.c_str());
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--help") {
        std::puts("vector_build --config build.json [--report report.json]\n"
                  "Builds the project-native graph segment and publishes CURRENT.");
        return 0;
    }
    try {
        return Run(argc, argv);
    } catch (const ConfigError& error) {
        std::fprintf(stderr, "Configuration error: %s\n", error.message.c_str());
        return 2;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "vector_build failed: %s\n", error.what());
        return 5;
    }
}
