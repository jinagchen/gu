#include "agent_io/synchronous_storage.hpp"

#include <atomic>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <tuple>

namespace agent_io {
namespace {

Error Invalid(const char* message) {
    return {ErrorCode::invalid_argument, message};
}
Error Exhausted() {
    return {ErrorCode::resource_exhausted, "Block reader capacity exhausted"};
}
Error Timeout() {
    return {ErrorCode::deadline_exceeded, "Block read deadline exceeded"};
}

struct KeyLess {
    bool operator()(const BlockKey& left, const BlockKey& right) const {
        return std::tie(left.file_id, left.aligned_offset, left.byte_count) <
               std::tie(right.file_id, right.aligned_offset, right.byte_count);
    }
};

class OwnedBlock final : public BlockLease {
    std::shared_ptr<const BudgetReservation> reservation_;
    std::shared_ptr<const FileLease> file_;
    std::shared_ptr<BufferLease> buffer_;
    BlockKey key_;
public:
    OwnedBlock(std::shared_ptr<const BudgetReservation> reservation,
               const BlockLocation& location, std::shared_ptr<BufferLease> buffer)
        : reservation_(std::move(reservation)), file_(location.file),
          buffer_(std::move(buffer)), key_(location.key) {}
    BlockKey Key() const noexcept override { return key_; }
    std::span<const std::byte> Bytes() const noexcept override {
        return buffer_->Bytes().first(key_.byte_count);
    }
};

// Every returned reference pins its batch output allocation charge as well.
class OutputBlock final : public BlockLease {
    std::shared_ptr<const BudgetReservation> reservation_;
    BlockLeasePtr block_;
public:
    OutputBlock(std::shared_ptr<const BudgetReservation> reservation, BlockLeasePtr block)
        : reservation_(std::move(reservation)), block_(std::move(block)) {}
    BlockKey Key() const noexcept override { return block_->Key(); }
    std::span<const std::byte> Bytes() const noexcept override { return block_->Bytes(); }
};

struct Pending {
    std::shared_ptr<const BudgetReservation> reservation;
    std::condition_variable ready;
    std::optional<Result<BlockLeasePtr>> result;
};

class Reader final : public BlockReader {
    std::shared_ptr<const BudgetReservation> reservation_;
    std::shared_ptr<ResourceGovernor> governor_;
    std::shared_ptr<SynchronousReadBackend> backend_;
    const std::uint32_t capacity_;
    std::mutex mutex_;
    std::condition_variable drained_;
    std::map<BlockKey, std::shared_ptr<Pending>, KeyLess> pending_;
    std::uint32_t calls_ = 0;
    bool closed_ = false;
    std::atomic<RequestId> next_request_{1};

    Result<Success> Validate(const BlockLocation& location) {
        const auto& key = location.key;
        if (!location.file || key.file_id != location.file->Id() ||
            !key.byte_count || key.aligned_offset > location.file->SizeBytes() ||
            key.byte_count > location.file->SizeBytes() - key.aligned_offset)
            return Invalid("Invalid block location");
        auto alignment = ReadAlignment(*location.file);
        if (const auto* error = std::get_if<Error>(&alignment)) return *error;
        const auto bytes = std::get<std::uint32_t>(alignment);
        if (key.aligned_offset % bytes || key.byte_count % bytes)
            return Invalid("Unaligned block location");
        return Success{};
    }

    Result<BlockLeasePtr> Load(const BlockLocation& location,
                              const std::shared_ptr<BufferLease>& storage) {
        auto charge = governor_->TryReserve(
            {MemoryRegion::workspace, sizeof(OwnedBlock) + 256});
        if (const auto* error = std::get_if<Error>(&charge)) return *error;
        auto id = next_request_.load();
        do {
            if (id == (std::numeric_limits<RequestId>::max)()) return Exhausted();
        } while (!next_request_.compare_exchange_weak(id, id + 1));
        std::optional<Result<ReadCompletion>> completion;
        auto submitted = backend_->Submit(
            {id, location.key, ReadPriority::demand, location.file, storage},
            [&](Result<ReadCompletion> result) { completion = std::move(result); });
        if (const auto* error = std::get_if<Error>(&submitted)) return *error;
        if (!completion)
            return Error{ErrorCode::io_error, "Synchronous backend omitted completion"};
        if (const auto* error = std::get_if<Error>(&*completion)) return *error;
        const auto done = std::get<ReadCompletion>(*completion);
        if (done.request_id != id || done.bytes_completed != location.key.byte_count)
            return Error{ErrorCode::io_error, "Invalid block read completion"};
        try {
            return BlockLeasePtr(std::make_shared<OwnedBlock>(
                std::get<std::shared_ptr<const BudgetReservation>>(std::move(charge)),
                location, storage));
        } catch (const std::bad_alloc&) {
            return Exhausted();
        }
    }

    Result<BlockLeasePtr> ReadOne(
        const BlockLocation& location, std::chrono::steady_clock::time_point deadline,
        const std::shared_ptr<BufferLease>& buffer) {
        std::shared_ptr<Pending> pending;
        {
            std::unique_lock lock(mutex_);
            if (const auto found = pending_.find(location.key); found != pending_.end()) {
                pending = found->second;
                if (!pending->ready.wait_until(lock, deadline,
                        [&] { return pending->result.has_value(); }))
                    return Timeout();
                return *pending->result;
            }
            if (pending_.size() >= capacity_) return Exhausted();
            auto charge = governor_->TryReserve(
                {MemoryRegion::metadata, sizeof(Pending) + sizeof(BlockKey) + 512});
            if (const auto* error = std::get_if<Error>(&charge)) return *error;
            try {
                pending = std::make_shared<Pending>();
                pending->reservation =
                    std::get<std::shared_ptr<const BudgetReservation>>(std::move(charge));
                pending_.emplace(location.key, pending);
            } catch (const std::bad_alloc&) {
                return Exhausted();
            }
        }
        // Synchronous kernel I/O runs outside the shared reader lock. A waiting
        // caller may time out; the leader retains all resources until completion.
        Result<BlockLeasePtr> result;
        try {
            result = Load(location, buffer);
        } catch (const std::bad_alloc&) {
            result = Exhausted();
        }
        {
            std::lock_guard lock(mutex_);
            pending->result.emplace(std::move(result));
            pending_.erase(location.key);
            pending->ready.notify_all();
        }
        return *pending->result;
    }

    struct Call {
        Reader& reader;
        ~Call() {
            std::lock_guard lock(reader.mutex_);
            --reader.calls_;
            reader.drained_.notify_all();
        }
    };
public:
    Reader(std::shared_ptr<ResourceGovernor> governor,
           std::shared_ptr<SynchronousReadBackend> backend, std::uint32_t capacity,
           std::shared_ptr<const BudgetReservation> reservation)
        : reservation_(std::move(reservation)), governor_(std::move(governor)),
          backend_(std::move(backend)), capacity_(capacity) {}

    Result<std::vector<BlockLeasePtr>> ReadBatch(
        std::span<const BlockLocation> locations, const BlockReadOptions& options) override {
        {
            std::lock_guard lock(mutex_);
            if (closed_) return Error{ErrorCode::closed, "Block reader drained"};
            if (calls_ >= capacity_) return Exhausted();
            ++calls_;
        }
        Call call{*this};
        if (locations.size() > capacity_ || options.priority == ReadPriority::prefetch)
            return Invalid("Invalid block batch");
        if (options.priority == ReadPriority::background)
            return Error{ErrorCode::unsupported, "Maintenance scheduling not implemented"};
        if (options.priority != ReadPriority::demand)
            return Invalid("Invalid block read priority");
        if (locations.empty()) return std::vector<BlockLeasePtr>{};
        for (const auto& location : locations) {
            auto valid = Validate(location);
            if (const auto* error = std::get_if<Error>(&valid)) return *error;
        }
        if (std::chrono::steady_clock::now() >= options.deadline) return Timeout();
        auto output_charge = governor_->TryReserve({MemoryRegion::workspace,
            locations.size() * (sizeof(BlockLeasePtr) +
                sizeof(std::shared_ptr<BufferLease>) + sizeof(OutputBlock) + 256)});
        if (const auto* error = std::get_if<Error>(&output_charge)) return *error;
        try {
            std::vector<BlockLeasePtr> output;
            output.reserve(locations.size());
            std::vector<std::shared_ptr<BufferLease>> buffers;
            buffers.reserve(locations.size());
            const auto charge =
                std::get<std::shared_ptr<const BudgetReservation>>(std::move(output_charge));
            // Reserve every output buffer before the first I/O; callers cannot
            // perform half a batch before discovering its payload does not fit.
            for (std::size_t i = 0; i < locations.size(); ++i) {
                std::shared_ptr<BufferLease> duplicate;
                for (std::size_t j = 0; j < i; ++j)
                    if (locations[j].key == locations[i].key) {
                        duplicate = buffers[j];
                        break;
                    }
                if (duplicate) {
                    buffers.push_back(std::move(duplicate));
                    continue;
                }
                auto buffer = AllocateReadBuffer(governor_, locations[i].key.byte_count,
                    std::get<std::uint32_t>(ReadAlignment(*locations[i].file)));
                if (const auto* error = std::get_if<Error>(&buffer)) return *error;
                buffers.push_back(std::get<std::shared_ptr<BufferLease>>(std::move(buffer)));
            }
            for (std::size_t i = 0; i < locations.size(); ++i) {
                if (std::chrono::steady_clock::now() >= options.deadline) return Timeout();
                // Same-batch duplicates reuse a lease even after the I/O completed.
                BlockLeasePtr existing;
                for (std::size_t j = 0; j < i; ++j) {
                    if (locations[j].key == locations[i].key) {
                        existing = output[j];
                        break;
                    }
                }
                if (existing) {
                    output.push_back(std::move(existing));
                    continue;
                }
                auto block = ReadOne(locations[i], options.deadline, buffers[i]);
                if (const auto* error = std::get_if<Error>(&block)) return *error;
                output.push_back(std::make_shared<OutputBlock>(
                    charge, std::get<BlockLeasePtr>(std::move(block))));
            }
            if (std::chrono::steady_clock::now() >= options.deadline) return Timeout();
            return output;
        } catch (const std::bad_alloc&) {
            return Exhausted();
        }
    }
    Result<PrefetchDisposition> TryPrefetch(const BlockLocation& location,
        std::chrono::steady_clock::time_point) override {
        std::lock_guard lock(mutex_);
        if (closed_) return Error{ErrorCode::closed, "Block reader drained"};
        auto valid = Validate(location);
        if (const auto* error = std::get_if<Error>(&valid)) return *error;
        return PrefetchDisposition::dropped;
    }
    Result<Success> Drain() override {
        std::unique_lock lock(mutex_);
        closed_ = true;
        drained_.wait(lock, [&] { return calls_ == 0; });
        lock.unlock();
        return backend_->Drain();
    }
};

}  // namespace

Result<std::unique_ptr<BlockReader>> CreateSynchronousBlockReader(
    std::shared_ptr<ResourceGovernor> governor,
    std::shared_ptr<SynchronousReadBackend> backend, std::uint32_t max_batch_blocks) {
    if (!governor || !backend || !max_batch_blocks)
        return Invalid("Invalid block reader options");
    auto reserved = governor->TryReserve({MemoryRegion::metadata, sizeof(Reader) + 256});
    if (const auto* error = std::get_if<Error>(&reserved)) return *error;
    try {
        return std::unique_ptr<BlockReader>(std::make_unique<Reader>(
            std::move(governor), std::move(backend), max_batch_blocks,
            std::get<std::shared_ptr<const BudgetReservation>>(std::move(reserved))));
    } catch (const std::bad_alloc&) {
        return Exhausted();
    }
}

}  // namespace agent_io
