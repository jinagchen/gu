#pragma once

// Real benchmark target: loads the published index through Engine and answers
// queries from the configured dataset. Every input hash is verified before the
// first measurement, so a measurement run cannot silently use other data.

#include "agent_io/benchmark_runner.hpp"
#include "agent_io/engine.hpp"

#include <filesystem>
#include <memory>
#include <vector>

namespace agent_io::bench_cli {

struct EngineTargetConfig {
    std::filesystem::path index_directory;
    std::filesystem::path vectors;
    std::filesystem::path ids;
    std::filesystem::path queries;
    std::filesystem::path query_ids;
    std::uint32_t dimension = 0;
    std::uint64_t query_count = 0;
    std::string vectors_sha256;
    std::string ids_sha256;
    std::string queries_sha256;
    std::string query_ids_sha256;
    EngineOptions engine;
};

class EngineBenchmarkTarget final : public BenchmarkTarget {
public:
    static Result<std::unique_ptr<EngineBenchmarkTarget>> Open(
        const EngineTargetConfig& config, std::shared_ptr<ResourceGovernor> governor);

    ~EngineBenchmarkTarget() override;
    VectorId QueryId(std::uint64_t row) const noexcept override;
    Result<BenchmarkSearchResult> Search(std::uint64_t row, const SearchOptions& options,
                                         std::span<Neighbor> output,
                                         std::stop_token stop) override;
    Result<EngineStats> Stats() const override;
    Result<Success> Drain(std::chrono::steady_clock::time_point deadline) override;

    std::uint64_t LoadNanoseconds() const noexcept { return load_ns_; }
    std::uint64_t UnbudgetedMetadataBytes() const noexcept { return unbudgeted_metadata_bytes_; }
    std::uint64_t ManifestGeneration() const noexcept { return manifest_generation_; }

private:
    EngineBenchmarkTarget() = default;

    std::unique_ptr<Engine> engine_;
    std::shared_ptr<const BudgetReservation> query_reservation_;
    std::vector<float> queries_;
    std::vector<VectorId> query_ids_;
    std::uint32_t dimension_ = 0;
    std::uint64_t query_count_ = 0;
    std::uint64_t load_ns_ = 0;
    std::uint64_t unbudgeted_metadata_bytes_ = 0;
    std::uint64_t manifest_generation_ = 0;
};

}  // namespace agent_io::bench_cli
