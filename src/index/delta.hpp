#pragma once

// Stage 6 B-side: the in-memory delta layer.
//
// A write is only one step: it becomes durable and visible by moving the
// publication prefix forward, and only after the WAL group that carries it has
// been synchronised. Two properties therefore matter more than throughput here.
//
// * A slot below the committed prefix never changes again, so a captured
//   ReadView can read it without a lock and without copying.
// * A slot is writable until it is published. A writer that fails before the
//   prefix moves must be able to take the slot back, otherwise a refused write
//   would leave a hole that later reads mistake for a record.
//
// The layer is deliberately a plain array: the delta is bounded by
// `max_retained_delta_vectors`, and a stable slot index keeps the publication
// path free of allocation.

#include "agent_io/index.hpp"
#include "agent_io/storage.hpp"

#include <memory>
#include <span>
#include <vector>

namespace agent_io {

struct DeltaOptions {
    std::uint32_t dimension = 0;
    // Hard ceiling on records held in one segment. Also the slot count.
    std::uint32_t slot_capacity = 0;
    // Bytes reserved per record, derived from the dimension, not estimated.
    std::uint64_t record_bytes = 0;
};

// Bytes a single record costs: the vector plus its id and version.
std::uint64_t DeltaRecordBytes(std::uint32_t dimension);

class DeltaLayer final : public DeltaStore {
public:
    static Result<std::unique_ptr<DeltaLayer>> Create(const DeltaOptions& options,
                                                      std::shared_ptr<ResourceGovernor> governor);

    // --- DeltaStore ------------------------------------------------------
    Result<SlotId> ReserveSlot() override;
    Result<Success> CancelLastSlot(SlotId slot) override;
    Result<Success> WriteSlot(SlotId slot, VectorId id, Sequence sequence,
                              std::span<const float> values) override;
    Result<Success> PublishPrefix(std::uint32_t new_prefix) override;
    DeltaPin CapturePin() const noexcept override;
    Result<DeltaSegmentInfo> Freeze() override;
    Result<DeltaSlot> SlotRecord(SlotId slot, const DeltaPin& pin) const override;
    Result<std::span<const float>> SlotValues(SlotId slot, const DeltaPin& pin) const override;

    // --- engine-side enumeration ----------------------------------------
    // Only the published prefix is visible; unpublished slots are invisible by
    // construction rather than filtered at every call site.
    std::uint32_t CommittedCount() const noexcept { return committed_; }
    VectorId IdAt(std::uint32_t index) const noexcept { return ids_[index]; }
    Sequence VersionAt(std::uint32_t index) const noexcept { return versions_[index]; }
    std::span<const float> ValuesAt(std::uint32_t index) const noexcept {
        return std::span<const float>(values_.data() +
                                          static_cast<std::size_t>(index) * options_.dimension,
                                      options_.dimension);
    }
    std::uint64_t ResidentBytes() const noexcept { return resident_bytes_; }
    std::uint32_t SlotCapacity() const noexcept { return options_.slot_capacity; }
    // Identifies the only segment this layer currently holds. A pin captured
    // before a reset names an older segment and must not be read again.
    SegmentId CurrentSegmentId() const noexcept { return segment_id_; }

    // Recovery path: a record read back from a durable WAL file is appended and
    // published immediately. The caller has already validated sequence
    // contiguity, so no cancellation window is needed here.
    Result<Success> RestoreRecord(VectorId id, Sequence sequence, std::span<const float> values);

    // Drop every record. Called after a checkpoint has made the delta durable
    // in a segment; the publication prefix must not depend on this layer again.
    void Reset();

private:
    explicit DeltaLayer(const DeltaOptions& options);

    DeltaOptions options_;
    std::shared_ptr<const BudgetReservation> reservation_;
    std::vector<VectorId> ids_;
    std::vector<Sequence> versions_;
    std::vector<float> values_;
    std::uint32_t reserved_ = 0;   // slots handed out, published or not
    std::uint32_t committed_ = 0;  // prefix visible to readers
    std::uint64_t resident_bytes_ = 0;
    SegmentId segment_id_ = 1;
    DeltaSegmentState state_ = DeltaSegmentState::active;
};

}  // namespace agent_io
