#pragma once

#include "agent_io/benchmark_runner.hpp"

#include <boost/json.hpp>

#include <filesystem>
#include <stdexcept>

namespace agent_io::bench_cli {

struct ConfigError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Config {
    BenchmarkOptions benchmark;
    MemoryBudget memory;
    std::uint64_t vector_count = 0;
    std::uint32_t dimension = 0;
    boost::json::object effective;
    // Engine target inputs. Present for every configuration; only used when the
    // real target runs.
    EngineOptions engine;
    std::filesystem::path directory;
    std::filesystem::path vectors;
    std::filesystem::path ids;
    std::filesystem::path queries;
    std::filesystem::path query_ids;
    std::string vectors_sha256;
    std::string ids_sha256;
    std::string queries_sha256;
    std::string query_ids_sha256;
    bool complete_evaluation_config = false;
};

std::string Utf8(const std::filesystem::path& path);
Config ReadConfig(const std::filesystem::path& path);
boost::json::object Counters(const BenchmarkOptions& options, const BenchmarkReport& report);
boost::json::object Summary(const BenchmarkOptions& options, const BenchmarkReport& report);

}  // namespace agent_io::bench_cli
