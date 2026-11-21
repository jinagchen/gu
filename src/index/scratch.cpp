#include "agent_io/index.hpp"

// RequiredScratch: every field is derived from the request parameters, never
// from a small-data measurement. The plan is an upper bound for one query and
// must fit the reservation the caller already holds.

#include <limits>

namespace agent_io {
namespace {

Error Invalid(std::string message) {
    return Error{ErrorCode::invalid_argument, std::move(message)};
}

bool Add(std::uint64_t& total, std::uint64_t part) {
    if (part > std::numeric_limits<std::uint64_t>::max() - total) return false;
    total += part;
    return true;
}

std::uint64_t Align64(std::uint64_t value) { return (value + 63u) & ~std::uint64_t{63u}; }

}  // namespace

Result<ScratchPlan> ComputeScratchPlan(const ScratchRequest& request) {
    const auto& budget = request.budget;
    if (request.dimension == 0 || request.dimension > 65536)
        return Invalid("Scratch: dimension is out of range");
    if (request.block_bytes == 0)
        return Invalid("Scratch: block_bytes is zero");
    if (budget.total_limit == 0 || budget.per_segment_limit == 0)
        return Invalid("Scratch: candidate limits must be positive");
    if (budget.delta_row_limit == 0)
        return Invalid("Scratch: delta row limit must be positive");
    if (request.delta_slots > budget.delta_row_limit)
        return Invalid("Scratch: delta slots exceed the declared limit");
    if (request.segment_count == 0 || request.segment_count > kMaxSegmentRefs)
        return Invalid("Scratch: segment count is out of range");
    if (request.max_degree == 0 || request.max_degree > 512)
        return Invalid("Scratch: max_degree is out of range");
    if (!request.pq_scratch_bytes.has_value())
        return Error{ErrorCode::unsupported,
                     "Scratch: upstream compressed-code scratch could not be derived"};
    if (!request.navigation_scratch_bytes.has_value())
        return Error{ErrorCode::unsupported,
                     "Scratch: navigation scratch could not be derived from index geometry"};

    ScratchPlan plan;
    plan.shadow_table_bytes = static_cast<std::uint64_t>(request.delta_slots) * 32;

    const std::uint64_t candidates =
        2ull * budget.total_limit +
        static_cast<std::uint64_t>(request.segment_count) * budget.per_segment_limit;
    const std::uint64_t candidate_bytes = candidates * sizeof(Candidate);
    plan.candidate_array_bytes = candidate_bytes;

    plan.refine_bytes = static_cast<std::uint64_t>(budget.total_limit) * request.dimension * 4;
    plan.pq_scratch_bytes = *request.pq_scratch_bytes;
    plan.navigation_bytes = *request.navigation_scratch_bytes;
    plan.io_buffer_bytes = static_cast<std::uint64_t>(request.copy_buffer_blocks) *
                           request.block_bytes;

    std::uint64_t total = 0;
    for (const std::uint64_t part : {plan.shadow_table_bytes, plan.candidate_array_bytes,
                                     plan.refine_bytes, plan.pq_scratch_bytes,
                                     plan.navigation_bytes, plan.io_buffer_bytes}) {
        if (!Add(total, part)) return Invalid("Scratch: byte total overflows");
    }
    const std::uint64_t aligned = Align64(total);
    if (aligned < total) return Invalid("Scratch: aligned total overflows");
    plan.total_bytes = aligned;
    if (plan.total_bytes > budget.workspace_bytes)
        return Error{ErrorCode::resource_exhausted,
                     "Scratch plan exceeds the per-query workspace reservation"};
    return plan;
}

}  // namespace agent_io
