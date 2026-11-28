#pragma once

// Graph construction shared by the first build and by maintenance segments.
//
// Both paths must produce the same kind of graph: nearest neighbours carry
// local accuracy, XOR lattice edges carry reachability. Keeping one
// implementation is what stops a checkpoint segment from being quietly
// unsearchable while the base segment stays fine.

#include "agent_io/index.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace agent_io {

struct GraphEdge {
    float distance = 0.0f;
    std::uint32_t node = 0;
};

// Exact k nearest neighbours plus symmetric long-range links. The work bound
// keeps a run predictable; exceeding it is a refusal, never a silent downgrade.
inline constexpr std::uint64_t kMaxAdjacencyWork = 4'000'000'000ull;

Result<std::vector<std::vector<GraphEdge>>> BuildAdjacency(
    std::span<const float> vectors, std::uint64_t count, std::uint32_t dimension,
    std::uint32_t max_degree);

}  // namespace agent_io
