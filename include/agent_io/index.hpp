#pragma once

// Internal contract for the index side (INDEX-B-1.1). Public Engine API stays at
// API-1.2; these declarations are for A/B integration only and are not a
// stable ABI. No implementation is supplied here.

#include "engine.hpp"
#include "storage.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace agent_io {

using SegmentId = std::uint64_t;
using NativeNodeId = std::uint32_t;
using SlotId = std::uint32_t;

// Bounded reference counts, aligned with ResourceLimits defaults so that a
// captured view never allocates on the publication path.
inline constexpr std::size_t kMaxSegmentRefs = 16;
inline constexpr std::size_t kMaxDeltaPins = 4;
inline constexpr std::size_t kMaxManifestSegments = 16;
inline constexpr std::size_t kSha256Bytes = 32;
inline constexpr std::size_t kInstanceIdBytes = 16;

using Digest = std::array<std::byte, kSha256Bytes>;
using InstanceId = std::array<std::byte, kInstanceIdBytes>;

enum class SegmentKind : std::uint8_t { base = 1, merged = 2, flushed_delta = 3 };

enum class DeltaSegmentState : std::uint8_t {
    active = 1,
    frozen = 2,
    retired = 3,
};

// Row layout inside one immutable segment. Layout is fixed per segment and
// validated on open; a mismatch is corruption, never silently re-interpreted.
struct NodeRecordLayout {
    std::uint32_t dimension = 0;
    std::uint32_t max_degree = 0;
    // Bytes occupied per node, including tail padding up to block alignment.
    std::uint32_t row_bytes = 0;
    std::uint32_t vector_offset = 0;
    std::uint32_t neighbor_offset = 0;
    std::uint32_t metadata_offset = 0;
};

struct SegmentDescriptor {
    SegmentId id = 0;
    Generation generation = 0;
    SegmentKind kind = SegmentKind::base;
    // Highest committed sequence whose updates are contained in this segment.
    Sequence covered_sequence = 0;
    std::uint64_t node_count = 0;
    std::uint64_t file_bytes = 0;
    std::uint32_t block_bytes = 0;
    NodeRecordLayout layout;
    // False marks unsupported artifacts; every queryable segment needs its
    // directory. A missing declared directory is corruption, never an empty ID set.
    bool has_version_directory = false;
    Digest content_sha256{};
};

// Native-node address inside a segment. block_index is derived from the row
// layout and the segment's block_bytes; it is not a byte offset so that a
// segment can be validated without trusting caller arithmetic.
struct NodeLocation {
    SegmentId segment = 0;
    NativeNodeId node = 0;
    std::uint64_t block_index = 0;
    std::uint32_t offset_in_block = 0;
    std::uint32_t bytes = 0;
};

struct IndexGeometry {
    std::uint32_t dimension = 0;
    Metric metric = Metric::squared_l2;
    std::uint32_t max_degree = 0;
    NativeNodeId entry_point = 0;
    SegmentId entry_segment = 0;
    // Upstream compressed-code size actually allocated per vector, in bytes.
    std::uint32_t pq_bytes_per_vector = 0;
    // Non-zero when the adapter could not route every read through
    // BlockReader; the value is the estimated bytes held outside the budget.
    std::uint64_t unbudgeted_metadata_bytes = 0;
};

// One candidate produced by graph expansion. compressed_distance only orders
// exploration and must never be reported as a result distance.
struct Candidate {
    VectorId id = 0;
    Sequence version = 0;
    SegmentId segment = 0;
    NativeNodeId node = 0;
    double compressed_distance = 0.0;
    std::optional<double> exact_distance;
};

// Cross-segment candidate ceiling. per_segment_limit applies before merging so
// one large segment cannot starve the others.
struct CandidateBudget {
    std::uint32_t total_limit = 128;
    std::uint32_t per_segment_limit = 64;
    std::uint32_t delta_row_limit = 12288;
    // Available per-query reservation, supplied before computing the plan.
    std::uint64_t workspace_bytes = 0;
};

// Non-owning capture boundary. Segment IDs are never reused within an instance.
struct DeltaPin {
    SegmentId segment = 0;
    // Slots below this prefix are immutable for the lifetime of the view.
    std::uint32_t committed_prefix = 0;
};

// Non-owning snapshot metadata. Consumers retain the accompanying ReadViewLease.
struct ReadView {
    InstanceId instance_id{};
    Generation view_generation = 0;
    Sequence snapshot_sequence = 0;
    Generation manifest_generation = 0;
    std::array<SegmentDescriptor, kMaxSegmentRefs> segments{};
    std::uint32_t segment_count = 0;
    std::array<DeltaPin, kMaxDeltaPins> deltas{};
    std::uint32_t delta_count = 0;
    std::uint64_t live_count = 0;
};

// B's concrete token retains disk registrations, delta stores and their budget
// charges. It is prepared before publication; copying a lease never allocates.
class ReadViewLifetime {
public:
    virtual ~ReadViewLifetime() = default;
};

struct ReadViewLease {
    ReadView view;
    std::shared_ptr<const ReadViewLifetime> lifetime;
};

struct DeltaSlot {
    SlotId slot;
    Sequence sequence;
    VectorId id;
    Sequence version;
};

struct DeltaSegmentInfo {
    SegmentId id = 0;
    DeltaSegmentState state = DeltaSegmentState::active;
    std::uint32_t slot_capacity = 0;
    std::uint32_t committed_prefix = 0;
    std::uint32_t frozen_prefix = 0;
    std::uint64_t bytes_reserved = 0;
};

// Stable-slot store. ReserveSlot hands out a slot without publishing it;
// PublishPrefix only moves forward and only under publication_mutex, so a
// captured prefix never changes underneath a query.
class DeltaStore {
public:
    virtual ~DeltaStore() = default;

    virtual Result<SlotId> ReserveSlot() = 0;
    // Single coordinator only, before WAL append: undo the most recent
    // unpublished reservation. Published slots can never be cancelled.
    virtual Result<Success> CancelLastSlot(SlotId slot) = 0;
    virtual Result<Success> WriteSlot(SlotId slot, VectorId id,
                                      Sequence sequence,
                                      std::span<const float> values) = 0;
    // Called under publication_mutex after WAL sync; no allocation on success.
    virtual Result<Success> PublishPrefix(std::uint32_t new_prefix) = 0;
    virtual DeltaPin CapturePin() const noexcept = 0;
    virtual Result<DeltaSegmentInfo> Freeze() = 0;
    virtual Result<DeltaSlot> SlotRecord(SlotId slot, const DeltaPin& pin) const = 0;
    virtual Result<std::span<const float>> SlotValues(
        SlotId slot, const DeltaPin& pin) const = 0;
};

enum class ExpansionState : std::uint8_t {
    more_available = 1,
    exhausted = 2,
    budget_exhausted = 3,
};

struct ExpansionRequest {
    std::span<const float> query;
    const ReadViewLease& view;
    CandidateBudget budget;
    std::uint32_t k = 0;
    std::chrono::steady_clock::time_point deadline;
    std::uint32_t prefetch_blocks = 0;
};

struct ExpansionResult {
    std::vector<Candidate> candidates;
    std::uint32_t blocks_read = 0;
    std::uint64_t bytes_read = 0;
    std::uint32_t prefetch_admitted = 0;
    std::uint32_t prefetch_consumed = 0;
    bool candidate_limit_reached = false;
    ExpansionState state = ExpansionState::exhausted;
};

struct RefinementRequest {
    std::span<const Candidate> candidates;
    std::span<const float> query;
    const ReadViewLease& view;
    CandidateBudget budget;
    std::chrono::steady_clock::time_point deadline;
};

// Graph adapter. Every byte read from disk must pass through BlockReader;
// Expand is synchronous from the caller's point of view even when the backend
// completes I/O asynchronously.
class IndexAdapter {
public:
    virtual ~IndexAdapter() = default;

    virtual Result<IndexGeometry> Geometry() const = 0;
    virtual Result<NodeLocation> Locate(SegmentId segment,
        NativeNodeId node, const ReadViewLease& view) const = 0;
    virtual Result<ExpansionResult> Expand(const ExpansionRequest& request) = 0;
    // Exact re-ranking over original vectors; filters versions above the view.
    virtual Result<std::vector<Candidate>> Refine(
        const RefinementRequest& request) = 0;
};

struct VersionDirectoryEntry {
    VectorId id = 0;
    Sequence version = 0;
    NativeNodeId node = 0;
};

// Per-segment ID -> highest version directory. Reads are budgeted through the
// cache; the whole directory is never pinned resident.
class VersionDirectory {
public:
    virtual ~VersionDirectory() = default;

    virtual Result<std::optional<VersionDirectoryEntry>> Find(VectorId id) = 0;
    virtual std::uint64_t EntryCount() const noexcept = 0;
};

// Per-query scratch upper bound. Every field must be derived from parameters,
// not estimated from a small-data run.
struct ScratchPlan {
    std::uint64_t shadow_table_bytes = 0;
    std::uint64_t candidate_array_bytes = 0;
    std::uint64_t refine_bytes = 0;
    std::uint64_t pq_scratch_bytes = 0;
    std::uint64_t navigation_bytes = 0;
    std::uint64_t io_buffer_bytes = 0;
    std::uint64_t total_bytes = 0;
};

struct ScratchRequest {
    std::uint32_t dimension = 0;
    CandidateBudget budget;
    std::uint32_t delta_slots = 0;
    std::uint32_t segment_count = 0;
    std::uint32_t max_degree = 0;
    // Includes demand copies as well as speculative copies, independent of
    // whether prefetch is enabled. BlockReader-owned buffers are separate.
    std::uint32_t copy_buffer_blocks = 0;
    std::uint32_t block_bytes = 0;
    // nullopt means unknown/unsupported. Zero requires proof of no allocation.
    std::optional<std::uint64_t> pq_scratch_bytes;
    // Bound for visited sets, graph traversal and upstream temporary storage,
    // derived from the opened index geometry, including its node counts.
    std::optional<std::uint64_t> navigation_scratch_bytes;
};

Result<ScratchPlan> ComputeScratchPlan(const ScratchRequest& request);

// First-build input. The build runs in its own Job object with
// memory_limit_bytes; staging_directory must be unique and ASCII-only.
struct BuildRequest {
    std::filesystem::path vectors_path;
    std::filesystem::path ids_path;
    std::string vectors_sha256;
    std::string ids_sha256;
    std::uint32_t dimension = 0;
    Metric metric = Metric::squared_l2;
    std::uint32_t max_degree = 32;
    std::uint32_t build_beam = 4;
    std::uint32_t pq_bytes = 64;
    std::uint64_t memory_limit_bytes = 0;
    std::uint64_t min_free_disk_bytes = 0;
    std::filesystem::path staging_directory;
    std::filesystem::path output_directory;
    // Upstream parameters are recorded verbatim, never re-derived later.
    std::string native_build_parameters;
};

struct BuildArtifacts {
    std::filesystem::path segment_file;
    std::filesystem::path version_directory_file;
    std::filesystem::path manifest_file;
    std::uint64_t node_count = 0;
    std::uint64_t file_bytes = 0;
    Digest content_sha256{};
    Digest version_directory_sha256{};
};

struct BuildOutcome {
    BuildArtifacts artifacts;
    std::uint64_t peak_commit_bytes = 0;
    std::uint64_t staged_bytes = 0;
    std::string native_version;
    bool published = false;
};

struct ManifestEntry {
    SegmentId id = 0;
    Generation generation = 0;
    SegmentKind kind = SegmentKind::base;
    Sequence covered_sequence = 0;
    std::uint64_t node_count = 0;
    std::uint64_t file_bytes = 0;
    Digest content_sha256{};
};

// Bounded, copyable manifest. Publishing replaces CURRENT only after this
// manifest and every referenced file are durable.
struct Manifest {
    InstanceId instance_id{};
    Generation generation = 0;
    Sequence checkpoint_sequence = 0;
    std::uint32_t dimension = 0;
    Metric metric = Metric::squared_l2;
    std::uint32_t block_bytes = 4096;
    NodeRecordLayout layout;
    std::array<ManifestEntry, kMaxManifestSegments> segments{};
    std::uint32_t segment_count = 0;
    std::uint64_t live_count = 0;
};

// Declaration-only manifest codecs; callers supply already reserved storage.
Result<std::uint64_t> ManifestBytes(const Manifest& manifest);
Result<std::uint64_t> EncodeManifest(const Manifest& manifest,
                                     std::span<std::byte> output);
Result<Manifest> DecodeManifest(std::span<const std::byte> input);

}  // namespace agent_io
