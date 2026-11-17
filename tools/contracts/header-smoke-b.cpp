#include "../../include/agent_io/engine.hpp"
#include "../../include/agent_io/index.hpp"
#include "../../include/agent_io/storage.hpp"

#include <type_traits>
#include <utility>

using namespace agent_io;

static_assert(sizeof(NativeNodeId) == 4);
static_assert(sizeof(SegmentId) == 8);
static_assert(sizeof(Digest) == 32);
static_assert(sizeof(DeltaSlot) == 32);

static_assert(std::is_abstract_v<DeltaStore>);
static_assert(std::is_abstract_v<IndexAdapter>);
static_assert(std::is_abstract_v<VersionDirectory>);

// A captured view must copy without allocating: it is taken under the
// publication mutex, where an allocation failure cannot be handled.
static_assert(std::is_trivially_copyable_v<ReadView>);
static_assert(!std::is_trivially_copyable_v<ReadViewLease>);
static_assert(std::is_nothrow_copy_constructible_v<ReadViewLease>);
static_assert(std::is_nothrow_destructible_v<ReadViewLease>);
static_assert(std::is_same_v<decltype(ReadViewLease{}.lifetime),
                             std::shared_ptr<const ReadViewLifetime>>);
static_assert(std::is_same_v<decltype(ExpansionRequest::view), const ReadViewLease&>);
static_assert(std::is_same_v<decltype(RefinementRequest::view), const ReadViewLease&>);
static_assert(std::is_same_v<decltype(ExpansionRequest::k), std::uint32_t>);
static_assert(std::is_same_v<decltype(RefinementRequest::budget), CandidateBudget>);
static_assert(std::is_same_v<decltype(ScratchRequest{}.navigation_scratch_bytes),
                             std::optional<std::uint64_t>>);
static_assert(std::is_trivially_copyable_v<Manifest>);
static_assert(sizeof(ReadView) < 4096);

// View capacity is the same constant the engine validates against, so the two
// cannot drift apart silently.
static_assert(ResourceLimits{}.max_disk_segments == kMaxSegmentRefs);
static_assert(EngineOptions{}.max_delta_segments == kMaxDeltaPins);
static_assert(kMaxManifestSegments >= kMaxSegmentRefs);

static_assert(std::is_same_v<decltype(std::declval<DeltaStore&>().ReserveSlot()),
                             Result<SlotId>>);
static_assert(std::is_same_v<decltype(std::declval<DeltaStore&>().CancelLastSlot(SlotId{})),
                             Result<Success>>);
static_assert(std::is_same_v<
              decltype(std::declval<DeltaStore&>().SlotValues(SlotId{}, DeltaPin{})),
              Result<std::span<const float>>>);
static_assert(std::is_same_v<
              decltype(std::declval<DeltaStore&>().WriteSlot(
                  SlotId{}, VectorId{}, Sequence{}, std::span<const float>{})),
              Result<Success>>);
static_assert(std::is_same_v<decltype(std::declval<const DeltaStore&>().CapturePin()),
                             DeltaPin>);
static_assert(noexcept(std::declval<const DeltaStore&>().CapturePin()));
static_assert(std::is_same_v<
              decltype(std::declval<const DeltaStore&>().SlotRecord(SlotId{}, DeltaPin{})),
              Result<DeltaSlot>>);
static_assert(std::is_same_v<
              decltype(std::declval<IndexAdapter&>().Locate(SegmentId{},
                      NativeNodeId{}, std::declval<const ReadViewLease&>())),
              Result<NodeLocation>>);
static_assert(std::is_same_v<
              decltype(std::declval<IndexAdapter&>().Expand(
                  std::declval<const ExpansionRequest&>())),
              Result<ExpansionResult>>);
static_assert(std::is_same_v<decltype(std::declval<IndexAdapter&>().Refine(
                  std::declval<const RefinementRequest&>())), Result<std::vector<Candidate>>>);
static_assert(std::is_same_v<decltype(ComputeScratchPlan(std::declval<const ScratchRequest&>())),
                             Result<ScratchPlan>>);
static_assert(std::is_same_v<decltype(DecodeManifest(
                                 std::declval<std::span<const std::byte>>())),
                             Result<Manifest>>);

// Rows must stay addressable inside one block-sized unit; a node spanning more
// than block_bytes is a configuration error, not a runtime surprise.
static_assert(std::is_trivially_copyable_v<NodeRecordLayout>);
static_assert(SegmentKind::base != SegmentKind::merged);
static_assert(ExpansionState::budget_exhausted != ExpansionState::exhausted);
