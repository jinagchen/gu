#include "delta.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace agent_io {
namespace {

constexpr std::uint64_t kIdBytes = sizeof(VectorId) + sizeof(Sequence);
constexpr std::uint64_t kSlotOverheadBytes = 32;  // slot bookkeeping per record

Error Invalid(std::string message) {
    return Error{ErrorCode::invalid_argument, std::move(message)};
}

bool AllFinite(std::span<const float> values) {
    for (const auto value : values) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        if ((bits & 0x7f800000u) == 0x7f800000u) return false;
    }
    return true;
}

}  // namespace

std::uint64_t DeltaRecordBytes(std::uint32_t dimension) {
    return static_cast<std::uint64_t>(dimension) * sizeof(float) + kIdBytes +
           kSlotOverheadBytes;
}

DeltaLayer::DeltaLayer(const DeltaOptions& options) : options_(options) {}

Result<std::unique_ptr<DeltaLayer>> DeltaLayer::Create(
    const DeltaOptions& options, std::shared_ptr<ResourceGovernor> governor) {
    if (options.dimension == 0)
        return Invalid("Delta dimension must be positive");
    if (options.slot_capacity == 0)
        return Invalid("Delta slot capacity must be positive");
    if (options.slot_capacity > std::numeric_limits<std::uint32_t>::max() / 2)
        return Invalid("Delta slot capacity is out of range");
    if (!governor) return Invalid("Delta needs a resource governor");

    DeltaOptions resolved = options;
    resolved.record_bytes = DeltaRecordBytes(resolved.dimension);
    auto layer = std::unique_ptr<DeltaLayer>(new DeltaLayer(resolved));

    const std::uint64_t reservation_bytes =
        static_cast<std::uint64_t>(resolved.slot_capacity) * resolved.record_bytes;
    auto reservation = governor->TryReserve(MemoryCharge{MemoryRegion::delta,
                                                         reservation_bytes});
    if (const auto* error = std::get_if<Error>(&reservation)) return *error;
    layer->reservation_ = std::get<std::shared_ptr<const BudgetReservation>>(
        std::move(reservation));

    const auto capacity = static_cast<std::size_t>(resolved.slot_capacity);
    layer->ids_.assign(capacity, 0);
    layer->versions_.assign(capacity, 0);
    layer->values_.assign(capacity * resolved.dimension, 0.0f);
    layer->resident_bytes_ = reservation_bytes;
    return layer;
}

Result<SlotId> DeltaLayer::ReserveSlot() {
    if (state_ != DeltaSegmentState::active)
        return Error{ErrorCode::resource_exhausted,
                     "Delta segment is frozen; checkpoint it before writing again"};
    if (reserved_ >= options_.slot_capacity)
        return Error{ErrorCode::resource_exhausted,
                     "Delta slot capacity exhausted; checkpoint is required"};
    return static_cast<SlotId>(reserved_++);
}

Result<Success> DeltaLayer::CancelLastSlot(SlotId slot) {
    if (slot >= reserved_) return Invalid("Slot was never reserved");
    if (slot < committed_) return Invalid("Published slots cannot be cancelled");
    if (slot + 1 != reserved_) return Invalid("Only the newest reserved slot can be cancelled");
    --reserved_;
    ids_[slot] = 0;
    versions_[slot] = 0;
    return Success{};
}

Result<Success> DeltaLayer::WriteSlot(SlotId slot, VectorId id, Sequence sequence,
                                      std::span<const float> values) {
    if (slot >= reserved_) return Invalid("Slot was never reserved");
    if (slot < committed_) return Invalid("Published slots are immutable");
    if (id == 0) return Invalid("Logical ID must be non-zero");
    if (sequence == 0) return Invalid("Delta record version must be non-zero");
    if (values.size() != options_.dimension) return Invalid("Delta vector dimension mismatch");
    if (!AllFinite(values)) return Invalid("Delta vector must contain finite float32 values");
    ids_[slot] = id;
    versions_[slot] = sequence;
    const auto offset = static_cast<std::size_t>(slot) * options_.dimension;
    for (std::uint32_t index = 0; index < options_.dimension; ++index)
        values_[offset + index] = values[index];
    return Success{};
}

Result<Success> DeltaLayer::PublishPrefix(std::uint32_t new_prefix) {
    if (state_ != DeltaSegmentState::active) return Invalid("Delta segment is frozen");
    if (new_prefix < committed_) return Invalid("The publication prefix cannot move backwards");
    if (new_prefix > reserved_) return Invalid("The prefix cannot exceed the reserved slots");
    committed_ = new_prefix;
    return Success{};
}

DeltaPin DeltaLayer::CapturePin() const noexcept {
    DeltaPin pin;
    pin.segment = segment_id_;
    pin.committed_prefix = committed_;
    return pin;
}

Result<DeltaSegmentInfo> DeltaLayer::Freeze() {
    if (state_ != DeltaSegmentState::active) return Invalid("Delta segment is already frozen");
    state_ = DeltaSegmentState::frozen;
    DeltaSegmentInfo info;
    info.id = segment_id_;
    info.state = DeltaSegmentState::frozen;
    info.slot_capacity = options_.slot_capacity;
    info.committed_prefix = committed_;
    info.frozen_prefix = committed_;
    info.bytes_reserved = resident_bytes_;
    return info;
}

Result<DeltaSlot> DeltaLayer::SlotRecord(SlotId slot, const DeltaPin& pin) const {
    if (pin.segment != segment_id_) return Invalid("The pin names a different delta segment");
    if (slot >= pin.committed_prefix) return Invalid("Slot is outside the committed prefix");
    DeltaSlot record;
    record.slot = slot;
    record.sequence = versions_[slot];
    record.id = ids_[slot];
    record.version = versions_[slot];
    return record;
}

Result<std::span<const float>> DeltaLayer::SlotValues(SlotId slot, const DeltaPin& pin) const {
    if (pin.segment != segment_id_) return Invalid("The pin names a different delta segment");
    if (slot >= pin.committed_prefix) return Invalid("Slot is outside the committed prefix");
    const auto offset = static_cast<std::size_t>(slot) * options_.dimension;
    return std::span<const float>(values_.data() + offset, options_.dimension);
}

Result<Success> DeltaLayer::RestoreRecord(VectorId id, Sequence sequence,
                                          std::span<const float> values) {
    if (state_ != DeltaSegmentState::active)
        return Error{ErrorCode::resource_exhausted,
                     "Delta segment is frozen; recovery cannot append"};
    if (reserved_ >= options_.slot_capacity)
        return Error{ErrorCode::resource_exhausted,
                     "Delta slot capacity exhausted during recovery"};
    const auto slot = static_cast<SlotId>(reserved_);
    ++reserved_;
    const auto written = WriteSlot(slot, id, sequence, values);
    if (const auto* error = std::get_if<Error>(&written)) {
        --reserved_;
        return *error;
    }
    return PublishPrefix(committed_ + 1);
}

Result<Success> DeltaLayer::RetirePrefix(std::uint32_t count) {
    if (state_ != DeltaSegmentState::active) return Invalid("Delta segment is not active");
    if (count > committed_) return Invalid("Cannot retire beyond the published prefix");
    if (count == 0) return Success{};
    const auto dimension = static_cast<std::size_t>(options_.dimension);
    const auto remaining = static_cast<std::size_t>(reserved_ - count);
    for (std::size_t index = 0; index < remaining; ++index) {
        const auto source = index + count;
        ids_[index] = ids_[source];
        versions_[index] = versions_[source];
        std::memmove(values_.data() + index * dimension, values_.data() + source * dimension,
                     dimension * sizeof(float));
    }
    for (std::size_t index = remaining; index < reserved_; ++index) {
        ids_[index] = 0;
        versions_[index] = 0;
    }
    reserved_ -= count;
    committed_ -= count;
    // Pins captured against the previous segment id must stop matching: their
    // slots no longer describe what is in the layer.
    ++segment_id_;
    return Success{};
}

void DeltaLayer::Reset() {
    state_ = DeltaSegmentState::active;
    ++segment_id_;
    reserved_ = 0;
    committed_ = 0;
    std::fill(ids_.begin(), ids_.end(), VectorId{0});
    std::fill(versions_.begin(), versions_.end(), Sequence{0});
}

}  // namespace agent_io
