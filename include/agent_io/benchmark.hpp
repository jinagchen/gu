#pragma once

#include "engine.hpp"

namespace agent_io {

// EVAL-1.1 value types shared by the producer and its consumers.
struct BenchmarkLimits {
    std::uint32_t workers = 1;
    std::uint32_t client_queue_capacity = 128;
    std::uint64_t client_queue_bytes = 1048576;
    std::uint32_t result_queue_capacity = 128;
    std::uint64_t result_queue_bytes = 1048576;
    std::uint32_t max_record_bytes = 65536;
};

enum class RequestStatus { ok, error, rejected };

struct RequestTiming {
    std::uint64_t scheduled_ns;
    std::optional<std::uint64_t> api_start_ns;
    std::uint64_t completed_ns;
};

struct RequestCounts {
    std::uint64_t scheduled = 0;
    std::uint64_t succeeded = 0;
    std::uint64_t failed = 0;
    std::uint64_t rejected = 0;
    std::uint64_t pending = 0;
    std::uint64_t window_completed = 0;
    std::uint64_t window_succeeded = 0;
};

}  // namespace agent_io
