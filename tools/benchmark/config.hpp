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
};

std::string Utf8(const std::filesystem::path& path);
Config ReadConfig(const std::filesystem::path& path);
boost::json::object Counters(const BenchmarkOptions& options, const BenchmarkReport& report);
boost::json::object Summary(const BenchmarkOptions& options, const BenchmarkReport& report);

}  // namespace agent_io::bench_cli
