#pragma once

// Graph construction shared by the first build and by maintenance segments.
//
// Both paths must produce the same kind of graph: nearest neighbours carry
// local accuracy, XOR lattice edges carry reachability. Keeping one
// implementation is what stops a checkpoint segment from being quietly
// unsearchable while the base segment stays fine.

#include "agent_io/index.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace agent_io {

struct GraphEdge {
    float distance = 0.0f;
    std::uint32_t node = 0;
};

using GraphAdjacency = std::vector<std::vector<GraphEdge>>;

// Exact k nearest neighbours plus symmetric long-range links. The work bound
// keeps a run predictable; exceeding it is a refusal, never a silent downgrade.
inline constexpr std::uint64_t kMaxAdjacencyWork = 4'000'000'000ull;

Result<GraphAdjacency> BuildAdjacency(
    std::span<const float> vectors, std::uint64_t count, std::uint32_t dimension,
    std::uint32_t max_degree);

Result<std::uint64_t> WriteGraphRows(
    const std::filesystem::path& path, const NodeRecordLayout& layout,
    std::span<const float> vectors, const GraphAdjacency& adjacency,
    const std::function<std::pair<VectorId, Sequence>(std::size_t)>& identity);

}  // namespace agent_io
