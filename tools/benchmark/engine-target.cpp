#include "engine-target.hpp"

#include "../../src/index/digest.hpp"
#include "../../src/index/reader.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>

namespace agent_io::bench_cli {
namespace {

using Clock = std::chrono::steady_clock;

Error Failure(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

Result<std::vector<std::byte>> ReadWhole(const std::filesystem::path& path,
                                         std::uint64_t expected_bytes) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return Failure(ErrorCode::io_error, "Cannot stat input: " + path.string());
    if (size != expected_bytes)
        return Failure(ErrorCode::invalid_argument,
                       "Input length does not match the configuration: " + path.string());
    std::ifstream input(path, std::ios::binary);
    if (!input) return Failure(ErrorCode::io_error, "Cannot open input: " + path.string());
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (size) input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!input) return Failure(ErrorCode::io_error, "Cannot read input: " + path.string());
    return bytes;
}

Result<Success> VerifyHash(const std::filesystem::path& path, const std::string& expected,
                           const char* name) {
    if (expected.empty())
        return Failure(ErrorCode::invalid_argument,
                       std::string("Measurement requires a SHA256 for ") + name);
    const auto digest = HashFile(path);
    if (const auto* error = std::get_if<Error>(&digest)) return *error;
    if (FormatDigest(std::get<Digest>(digest)) != expected)
        return Failure(ErrorCode::corruption,
                       std::string("Input hash mismatch for ") + name);
    return Success{};
}

}  // namespace

EngineBenchmarkTarget::~EngineBenchmarkTarget() = default;

Result<std::unique_ptr<EngineBenchmarkTarget>> EngineBenchmarkTarget::Open(
    const EngineTargetConfig& config, std::shared_ptr<ResourceGovernor> governor) {
    if (config.dimension == 0 || config.query_count == 0)
        return Failure(ErrorCode::invalid_argument, "Engine target needs queries and a dimension");
    if (!governor) return Failure(ErrorCode::invalid_argument, "Engine target needs a governor");
    const auto started = Clock::now();
    if (auto status = VerifyHash(config.vectors, config.vectors_sha256, "dataset.vectors");
        !std::holds_alternative<Success>(status))
        return std::get<Error>(status);
    if (auto status = VerifyHash(config.ids, config.ids_sha256, "dataset.ids");
        !std::holds_alternative<Success>(status))
        return std::get<Error>(status);
    if (auto status = VerifyHash(config.queries, config.queries_sha256, "dataset.queries");
        !std::holds_alternative<Success>(status))
        return std::get<Error>(status);
    if (auto status = VerifyHash(config.query_ids, config.query_ids_sha256, "dataset.query_ids");
        !std::holds_alternative<Success>(status))
        return std::get<Error>(status);

    auto query_bytes = ReadWhole(config.queries,
                                 config.query_count * config.dimension * 4);
    if (const auto* error = std::get_if<Error>(&query_bytes)) return *error;
    auto id_bytes = ReadWhole(config.query_ids, config.query_count * 8);
    if (const auto* error = std::get_if<Error>(&id_bytes)) return *error;

    auto target = std::unique_ptr<EngineBenchmarkTarget>(new EngineBenchmarkTarget());
    target->dimension_ = config.dimension;
    target->query_count_ = config.query_count;
    const auto& query_raw = std::get<std::vector<std::byte>>(query_bytes);
    target->queries_.resize(query_raw.size() / sizeof(float));
    std::memcpy(target->queries_.data(), query_raw.data(), query_raw.size());
    const auto& id_raw = std::get<std::vector<std::byte>>(id_bytes);
    target->query_ids_.resize(id_raw.size() / sizeof(std::uint64_t));
    std::memcpy(target->query_ids_.data(), id_raw.data(), id_raw.size());

    const std::uint64_t query_bytes_total = query_raw.size() + id_raw.size();
    auto reservation = governor->TryReserve({MemoryRegion::workspace, query_bytes_total});
    if (const auto* error = std::get_if<Error>(&reservation)) return *error;
    target->query_reservation_ =
        std::get<std::shared_ptr<const BudgetReservation>>(reservation);

    auto engine = Engine::Open(config.index_directory, config.engine);
    if (const auto* error = std::get_if<Error>(&engine)) return *error;
    target->engine_ = std::get<std::unique_ptr<Engine>>(std::move(engine));
    const auto stats = target->engine_->Stats();
    if (const auto* error = std::get_if<Error>(&stats)) return *error;
    if (std::get<EngineStats>(stats).live_vectors == 0)
        return Failure(ErrorCode::corruption, "Index publishes no live vectors");
    const auto description = DescribeIndex(config.index_directory);
    if (const auto* error = std::get_if<Error>(&description)) return *error;
    target->unbudgeted_metadata_bytes_ =
        std::get<IndexDescription>(description).unbudgeted_metadata_bytes;
    target->manifest_generation_ = std::get<IndexDescription>(description).generation;
    target->load_ns_ = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
    return target;
}

VectorId EngineBenchmarkTarget::QueryId(std::uint64_t row) const noexcept {
    return row < query_ids_.size() ? query_ids_[row] : 0;
}

Result<BenchmarkSearchResult> EngineBenchmarkTarget::Search(std::uint64_t row,
                                                            const SearchOptions& options,
                                                            std::span<Neighbor> output,
                                                            std::stop_token stop) {
    if (row >= query_count_)
        return Failure(ErrorCode::invalid_argument, "Query row is outside the dataset");
    if (stop.stop_requested())
        return Failure(ErrorCode::deadline_exceeded, "Benchmark cancellation requested");
    const std::span<const float> query(queries_.data() + row * dimension_, dimension_);
    auto result = engine_->Search(query, options);
    if (const auto* error = std::get_if<Error>(&result)) return *error;
    const auto& search = std::get<SearchResult>(result);
    const auto expected = (std::min)(static_cast<std::size_t>(options.k),
                                     static_cast<std::size_t>(search.neighbors.size()));
    if (expected > output.size())
        return Failure(ErrorCode::invalid_argument, "Result slot is smaller than K");
    std::copy_n(search.neighbors.begin(), expected, output.begin());
    const auto stats = engine_->Stats();
    if (const auto* error = std::get_if<Error>(&stats)) return *error;
    const auto live = std::get<EngineStats>(stats).live_vectors;
    const auto required = (std::min)(static_cast<std::uint64_t>(options.k), live);
    if (expected < required)
        return Failure(ErrorCode::search_incomplete,
                       "Graph search returned fewer neighbours than required");
    BenchmarkSearchResult summary;
    summary.snapshot_sequence = search.snapshot_sequence;
    summary.view_generation = search.view_generation;
    summary.neighbor_count = static_cast<std::uint32_t>(expected);
    return summary;
}

Result<EngineStats> EngineBenchmarkTarget::Stats() const { return engine_->Stats(); }

Result<Success> EngineBenchmarkTarget::Drain(std::chrono::steady_clock::time_point) {
    // The synchronous backend completes every read before returning, so there
    // is no outstanding I/O to quiesce; admission stays open for later phases.
    return Success{};
}

}  // namespace agent_io::bench_cli
