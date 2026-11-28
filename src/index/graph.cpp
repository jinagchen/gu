#include "graph.hpp"
#include "format.hpp"

#include <algorithm>
#include <fstream>

namespace agent_io {
namespace {

double SquaredL2(const float* left, const float* right, std::uint32_t dimension) {
    double total = 0.0;
    for (std::uint32_t index = 0; index < dimension; ++index) {
        const double delta = static_cast<double>(left[index]) - static_cast<double>(right[index]);
        total += delta * delta;
    }
    return total;
}

}  // namespace

Result<GraphAdjacency> BuildAdjacency(
    std::span<const float> vectors, std::uint64_t count, std::uint32_t dimension,
    std::uint32_t max_degree) {
    if (count < 2)
        return std::vector<std::vector<GraphEdge>>(static_cast<std::size_t>(count));
    if (dimension == 0)
        return Error{ErrorCode::invalid_argument, "Graph dimension must be positive"};
    if (count > kMaxAdjacencyWork / dimension / count)
        return Error{ErrorCode::unsupported,
                     "Exact graph construction exceeds the work bound; use a smaller "
                     "fixture or connect the native builder"};
    // The degree budget is split: nearest neighbours carry local accuracy, the
    // remaining slots carry the long-range links that make the graph navigable.
    // Reserving the slots up front matters, because trimming the adjacency list
    // to max_degree by distance would drop every long-range link again.
    std::uint32_t long_range = (std::max)(2u, max_degree / 4);
    if (long_range >= max_degree) long_range = max_degree / 2;
    const std::uint32_t nearest = max_degree - long_range;
    std::vector<std::vector<GraphEdge>> graph(static_cast<std::size_t>(count));
    std::vector<GraphEdge> best;
    best.reserve(nearest + 1);
    const auto farther = [](const GraphEdge& left, const GraphEdge& right) {
        return left.distance < right.distance;
    };
    for (std::uint64_t node = 0; node < count; ++node) {
        best.clear();
        const float* base = vectors.data() + node * dimension;
        for (std::uint64_t other = 0; other < count; ++other) {
            if (other == node) continue;
            const auto distance = static_cast<float>(
                SquaredL2(base, vectors.data() + other * dimension, dimension));
            const GraphEdge candidate{distance, static_cast<std::uint32_t>(other)};
            if (best.size() < nearest) {
                best.push_back(candidate);
                std::push_heap(best.begin(), best.end(), farther);
            } else if (candidate.distance < best.front().distance) {
                std::pop_heap(best.begin(), best.end(), farther);
                best.back() = candidate;
                std::push_heap(best.begin(), best.end(), farther);
            }
        }
        std::sort(best.begin(), best.end(), farther);
        graph[static_cast<std::size_t>(node)] = best;
    }
    // Reverse edges keep the graph strongly connected for nodes that no one
    // selected; duplicates are removed and each list is re-trimmed by distance.
    for (std::uint64_t node = 0; node < count; ++node) {
        for (const auto& edge : graph[static_cast<std::size_t>(node)]) {
            auto& reverse = graph[edge.node];
            const auto duplicate = std::find_if(reverse.begin(), reverse.end(),
                [node](const GraphEdge& item) { return item.node == node; });
            if (duplicate != reverse.end()) continue;
            const float* left = vectors.data() + static_cast<std::size_t>(edge.node) * dimension;
            const float* right = vectors.data() + node * dimension;
            reverse.push_back(GraphEdge{static_cast<float>(SquaredL2(left, right, dimension)),
                                        static_cast<std::uint32_t>(node)});
        }
    }
    for (std::uint64_t node = 0; node < count; ++node) {
        auto& edges = graph[static_cast<std::size_t>(node)];
        std::sort(edges.begin(), edges.end(), [](const GraphEdge& left, const GraphEdge& right) {
            if (left.distance != right.distance) return left.distance < right.distance;
            return left.node < right.node;
        });
        edges.erase(std::unique(edges.begin(), edges.end(),
            [](const GraphEdge& left, const GraphEdge& right) { return left.node == right.node; }),
            edges.end());
        if (edges.size() > nearest) edges.resize(nearest);
    }
    // Long-range links: an exact kNN graph is disconnected when clusters are
    // tight, and a disconnected graph cannot be searched from one entry point.
    // XOR-with-powers-of-two edges form a symmetric hypercube lattice over the
    // node ids, so every node is reachable in O(log count) hops from the entry
    // point without pretending to be Vamana pruning.
    for (std::uint64_t node = 0; node < count; ++node) {
        auto& edges = graph[static_cast<std::size_t>(node)];
        for (std::uint64_t offset = 1; offset < count && edges.size() < max_degree;
             offset <<= 1) {
            const auto candidate = static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(node) ^ static_cast<std::uint32_t>(offset));
            if (candidate >= count || candidate == node) continue;
            const bool present = std::any_of(edges.begin(), edges.end(),
                [candidate](const GraphEdge& item) { return item.node == candidate; });
            if (present) continue;
            const float* left = vectors.data() + static_cast<std::size_t>(node) * dimension;
            const float* right = vectors.data() + static_cast<std::size_t>(candidate) * dimension;
            edges.push_back(GraphEdge{static_cast<float>(SquaredL2(left, right, dimension)),
                                      static_cast<std::uint32_t>(candidate)});
        }
    }
    return graph;
}

Result<std::uint64_t> WriteGraphRows(
    const std::filesystem::path& path, const NodeRecordLayout& layout,
    std::span<const float> vectors, const GraphAdjacency& adjacency,
    const std::function<std::pair<VectorId, Sequence>(std::size_t)>& identity) {
    if (!layout.dimension || vectors.size() % layout.dimension ||
        adjacency.size() != vectors.size() / layout.dimension)
        return Error{ErrorCode::invalid_argument, "Graph rows disagree with vector data"};
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return Error{ErrorCode::io_error, "Cannot open the staged graph"};
    std::vector<std::byte> row(layout.row_bytes);
    NodeRow record;
    for (std::size_t node = 0; node < adjacency.size(); ++node) {
        const auto [id, version] = identity(node);
        record.id = id;
        record.version = version;
        record.neighbors.clear();
        for (const auto& edge : adjacency[node]) record.neighbors.push_back(edge.node);
        const auto encoded = EncodeNodeRow(
            record, vectors.subspan(node * layout.dimension, layout.dimension), layout, row);
        if (const auto* error = std::get_if<Error>(&encoded)) return *error;
        output.write(reinterpret_cast<const char*>(row.data()),
                     static_cast<std::streamsize>(row.size()));
        if (!output) return Error{ErrorCode::io_error, "Cannot write the graph segment"};
    }
    output.close();
    if (!output) return Error{ErrorCode::io_error, "Cannot close the staged graph"};
    return static_cast<std::uint64_t>(layout.row_bytes) * adjacency.size();
}

}  // namespace agent_io
