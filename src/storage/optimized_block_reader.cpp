#include "agent_io/optimized_storage.hpp"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <tuple>
#include <utility>

namespace agent_io {
namespace {

using Clock = std::chrono::steady_clock;
using Reservation = std::shared_ptr<const BudgetReservation>;
thread_local std::shared_ptr<QueryReadState> current_query;

Error Exhausted() { return {ErrorCode::resource_exhausted, "Read budget exhausted"}; }
Error Timeout() { return {ErrorCode::deadline_exceeded, "Block read deadline exceeded"}; }
Error Closed() { return {ErrorCode::closed, "Block reader drained"}; }

struct KeyLess {
    bool operator()(const BlockKey& a, const BlockKey& b) const {
        return std::tie(a.file_id, a.aligned_offset, a.byte_count) <
               std::tie(b.file_id, b.aligned_offset, b.byte_count);
    }
};

Result<std::uint32_t> Validate(const BlockLocation& location) {
    const auto& [id, offset, size] = location.key;
    if (!location.file || id != location.file->Id() || !size ||
        offset > location.file->SizeBytes() || size > location.file->SizeBytes() - offset)
        return Error{ErrorCode::invalid_argument, "Invalid block location"};
    auto alignment = ReadAlignment(*location.file);
    if (const auto* align = std::get_if<std::uint32_t>(&alignment)) {
        if (offset % *align || size % *align)
            return Error{ErrorCode::invalid_argument, "Unaligned block location"};
    }
    return alignment;
}

class Block final : public BlockLease {
    Reservation reservation_;
public:
    const BlockLocation location;
    const std::shared_ptr<BufferLease> buffer;
    const bool cacheable;
    const bool speculative;
    bool consumed = false;  // Protected by the reader mutex.

    Block(Reservation reservation, const BlockLocation& at, std::shared_ptr<BufferLease> bytes,
          bool cache, bool prefetch)
        : reservation_(std::move(reservation)), location(at), buffer(std::move(bytes)),
          cacheable(cache), speculative(prefetch) {}
    BlockKey Key() const noexcept override { return location.key; }
    std::span<const std::byte> Bytes() const noexcept override { return buffer->Bytes(); }
};

struct BatchPin {
    Reservation reservation;
    std::shared_ptr<QueryReadState> query;
    ReadPriority priority;
    Clock::time_point deadline;
};

struct WaitPin {
    std::shared_ptr<BatchPin> batch;
    bool waiting = true;
};

using WaitPins = std::list<WaitPin>;

class OutputBlock final : public BlockLease {
    Reservation reservation_;
    std::shared_ptr<const Block> block_;
public:
    OutputBlock(Reservation reservation, std::shared_ptr<const Block> block)
        : reservation_(std::move(reservation)), block_(std::move(block)) {}
    BlockKey Key() const noexcept override { return block_->Key(); }
    std::span<const std::byte> Bytes() const noexcept override { return block_->Bytes(); }
};

struct Pending {
    Reservation reservation;
    std::shared_ptr<Block> block;
    RequestId id = 0;
    ReadPriority priority = ReadPriority::demand;
    bool dispatched = false;
    bool done = false;
    bool submission_counted = false;
    std::optional<Error> error;
    std::shared_ptr<QueryReadState> prefetch_query;
    WaitPins pins;
};

class Reader final : public OptimizedBlockReader {
    struct Cached {
        std::shared_ptr<Block> block;
        bool touched;
    };
    using Cache = std::map<BlockKey, Cached, KeyLess>;
    using PendingMap = std::map<BlockKey, std::shared_ptr<Pending>, KeyLess>;
    struct Item {
        std::shared_ptr<Block> block;
        std::shared_ptr<Pending> pending;
        WaitPins::iterator pin;
        bool hit = false;
    };
    struct Call {
        Reader& reader;
        bool background = false;
        ~Call() {
            std::lock_guard lock(reader.mutex_);
            --reader.calls_;
            if (background) --reader.background_calls_;
            reader.changed_.notify_all();
        }
    };

    Reservation reservation_;
    std::shared_ptr<ResourceGovernor> governor_;
    std::shared_ptr<MeteredReadBackend> backend_;
    const bool inline_backend_;
    const OptimizedReaderOptions options_;
    mutable std::mutex mutex_;
    std::mutex drain_mutex_;
    std::condition_variable changed_;
    Cache cache_;
    Cache::iterator hand_ = cache_.end();
    PendingMap pending_;
    BlockReaderStats stats_;
    RequestId next_id_ = 1;
    std::uint64_t calls_ = 0;
    std::uint32_t background_calls_ = 0;
    std::uint32_t active_reads_ = 0;
    std::uint32_t prefetch_inflight_ = 0;
    std::uint32_t background_inflight_ = 0;
    std::uint32_t demand_burst_ = 0;
    bool closed_ = false;

    Cache::iterator Remove(Cache::iterator it) {
        if (hand_ == it) ++hand_;
        stats_.cache_resident_bytes -= it->first.byte_count;
        return cache_.erase(it);
    }

    bool Evict() {
        // Pinned blocks cost the same after eviction. Leave them usable and
        // give demand reads a workspace fallback if all cache bytes are pinned.
        const auto scans = cache_.size() * 2;
        for (std::size_t i = 0; i < scans; ++i) {
            if (hand_ == cache_.end()) hand_ = cache_.begin();
            const auto candidate = hand_++;
            if (candidate->second.block.use_count() != 1) continue;
            if (std::exchange(candidate->second.touched, false)) continue;
            Remove(candidate);
            return true;
        }
        return false;
    }

    Result<std::shared_ptr<Block>> Allocate(const BlockLocation& at, ReadPriority priority) {
        const bool prefetch = priority == ReadPriority::prefetch;
        const auto alignment = std::get<std::uint32_t>(ReadAlignment(*at.file));
        const auto allocate_in = [&](MemoryRegion region) -> Result<std::shared_ptr<Block>> {
            auto charge = governor_->TryReserve({region, sizeof(Block) + 512});
            if (const auto* error = std::get_if<Error>(&charge)) return *error;
            auto buffer = AllocateReadBuffer(governor_, at.key.byte_count, alignment, region);
            if (const auto* error = std::get_if<Error>(&buffer)) return *error;
            return std::make_shared<Block>(std::get<Reservation>(std::move(charge)), at,
                std::get<std::shared_ptr<BufferLease>>(std::move(buffer)),
                region == MemoryRegion::cache, prefetch);
        };
        // A maintenance scan may share an existing block but never evicts the query cache.
        if (priority != ReadPriority::background &&
            options_.cache_enabled && !ReadFileRetired(*at.file)) {
            do {
                auto block = allocate_in(MemoryRegion::cache);
                if (std::holds_alternative<std::shared_ptr<Block>>(block)) return block;
            } while (!prefetch && Evict());
        }
        if (prefetch) return Exhausted();
        return allocate_in(MemoryRegion::workspace);
    }

    Result<std::shared_ptr<Pending>> Prepare(const BlockLocation& at, ReadPriority priority) {
        if (next_id_ == (std::numeric_limits<RequestId>::max)()) return Exhausted();
        auto charge = governor_->TryReserve({MemoryRegion::metadata, sizeof(Pending) + 512});
        if (const auto* error = std::get_if<Error>(&charge)) return *error;
        auto block = Allocate(at, priority);
        if (const auto* error = std::get_if<Error>(&block)) return *error;
        auto pending = std::make_shared<Pending>();
        pending->reservation = std::get<Reservation>(std::move(charge));
        pending->block = std::get<std::shared_ptr<Block>>(std::move(block));
        pending->id = next_id_++;
        return pending;
    }

    void CountAccepted(Pending& pending) {
        if (std::exchange(pending.submission_counted, true)) return;
        if (pending.priority == ReadPriority::prefetch) ++stats_.prefetch_submitted;
        if (pending.priority == ReadPriority::background) ++stats_.background_submitted;
    }

    void Complete(const std::shared_ptr<Pending>& pending, Result<ReadCompletion> completion,
                  bool accepted) {
        std::lock_guard lock(mutex_);
        const auto& block = pending->block;
        const auto key = block->Key();
        if (accepted) CountAccepted(*pending);
        if (auto* error = std::get_if<Error>(&completion)) pending->error = std::move(*error);
        else {
            const auto result = std::get<ReadCompletion>(completion);
            if (result.request_id != pending->id || result.bytes_completed != key.byte_count)
                pending->error = Error{ErrorCode::io_error, "Invalid block read completion"};
        }
        if (!pending->error && block->cacheable && !ReadFileRetired(*block->location.file)) {
            try {
                if (cache_.emplace(key, Cached{block, !block->speculative}).second)
                    stats_.cache_resident_bytes += key.byte_count;
            } catch (const std::bad_alloc&) {
                // The completed immutable block is still valid for its waiters.
            }
        }
        if (pending->priority == ReadPriority::prefetch) {
            --prefetch_inflight_;
            if (pending->prefetch_query) --pending->prefetch_query->outstanding_prefetch;
            pending->prefetch_query.reset();
        }
        if (pending->priority == ReadPriority::background) --background_inflight_;
        pending->done = true;
        --active_reads_;
        pending->pins.clear();
        pending_.erase(key);
        changed_.notify_all();
    }

    bool Submit(const std::shared_ptr<Pending>& pending) {
        const auto& block = pending->block;
        Result<Success> submitted;
        std::optional<Result<ReadCompletion>> inline_result;
        try {
            ReadCallback callback;
            if (inline_backend_) {
                callback = [&](Result<ReadCompletion> completion) {
                    inline_result.emplace(std::move(completion));
                };
            } else {
                callback = [this, pending](Result<ReadCompletion> completion) {
                    Complete(pending, std::move(completion), true);
                };
            }
            submitted = backend_->Submit(
                {pending->id, block->Key(), pending->priority, block->location.file, block->buffer},
                std::move(callback));
        } catch (const std::bad_alloc&) {
            submitted = Error{ErrorCode::resource_exhausted, {}};
        }
        if (auto* error = std::get_if<Error>(&submitted)) {
            Complete(pending, std::move(*error), false);
            return false;
        }
        if (inline_backend_) {
            if (inline_result) Complete(pending, std::move(*inline_result), true);
            else Complete(pending, Error{ErrorCode::io_error, "Missing inline completion"}, false);
        } else if (pending->priority != ReadPriority::demand) {
            std::lock_guard lock(mutex_);
            CountAccepted(*pending);
        }
        return true;
    }

    void NoteDepth() {
        stats_.peak_inflight_reads = (std::max)(stats_.peak_inflight_reads, active_reads_);
        stats_.peak_prefetch_reads = (std::max)(stats_.peak_prefetch_reads, prefetch_inflight_);
        stats_.peak_background_reads = (std::max)(stats_.peak_background_reads, background_inflight_);
    }

    static bool HasWaiter(const Pending& pending, ReadPriority priority, Clock::time_point now) {
        return std::ranges::any_of(pending.pins, [&](const auto& pin) {
            return pin.waiting && pin.batch->priority == priority && now < pin.batch->deadline;
        });
    }

    std::size_t QueuedBackground(Clock::time_point now, bool first_only = false) const {
        if (!background_calls_) return 0;
        std::size_t count = 0;
        for (const auto& entry : pending_) {
            const auto& pending = entry.second;
            if (!pending->dispatched && HasWaiter(*pending, ReadPriority::background, now)) {
                ++count;
                if (first_only) break;
            }
        }
        return count;
    }

    void Detach(const std::vector<Item>& items) {
        bool queued = false;
        for (const auto& item : items) {
            const auto& pending = item.pending;
            if (!pending || pending->done) continue;
            if (pending->dispatched && item.pin->batch->query) {
                // Keep the query pin until completion or until dispatch is revoked.
                item.pin->waiting = false;
                continue;
            }
            pending->pins.erase(item.pin);
            if (!pending->dispatched) {
                queued = true;
                if (pending->pins.empty()) pending_.erase(pending->block->Key());
            }
        }
        if (queued) changed_.notify_all();
    }

    Result<Success> Dispatch(const std::vector<Item>& items, Clock::time_point deadline,
                             std::unique_lock<std::mutex>& lock) {
        for (;;) {
            const auto now = Clock::now();
            if (now >= deadline) return Timeout();
            std::shared_ptr<Pending> next;
            auto priority = ReadPriority::demand;
            bool waiting = false;
            bool demand_allowed = active_reads_ < options_.max_inflight_reads;
            bool background_allowed = false;
            if (const auto& scheduling = options_.scheduling) {
                const bool queued = QueuedBackground(now, true) != 0;
                background_allowed = queued && demand_allowed &&
                    background_inflight_ < scheduling->max_background_reads &&
                    background_inflight_ + prefetch_inflight_ <
                        options_.max_inflight_reads - scheduling->demand_reserve;
                if (!queued) demand_burst_ = 0;
                if (queued || background_inflight_) {
                    demand_allowed = demand_allowed &&
                        active_reads_ - background_inflight_ <
                            options_.max_inflight_reads - scheduling->background_reserve;
                }
                if (background_allowed && demand_burst_ >= scheduling->demand_burst_before_background)
                    demand_allowed = false;
            }
            for (const auto& item : items) {
                const auto& pending = item.pending;
                if (!pending) continue;
                if (pending->error) return *pending->error;
                if (pending->done) continue;
                waiting = true;
                if (pending->dispatched || next) continue;
                if (demand_allowed &&
                    (!background_calls_ || HasWaiter(*pending, ReadPriority::demand, now))) {
                    next = pending;
                } else if (background_allowed && HasWaiter(*pending, ReadPriority::background, now)) {
                    next = pending;
                    priority = ReadPriority::background;
                }
            }
            if (!waiting) return Success{};
            if (!next) {
                changed_.wait_until(lock, deadline);
                continue;
            }

            if (Clock::now() >= deadline) return Timeout();
            next->priority = priority;
            next->dispatched = true;
            ++active_reads_;
            if (priority == ReadPriority::background) {
                ++background_inflight_;
                demand_burst_ = 0;
            } else if (background_allowed) {
                ++demand_burst_;
            }
            NoteDepth();
            // A different batch may now own the next eligible key.
            if (background_calls_) changed_.notify_all();
            lock.unlock();
            if (Clock::now() >= deadline) {
                lock.lock();
                --active_reads_;
                if (priority == ReadPriority::background) --background_inflight_;
                next->dispatched = false;
                next->pins.remove_if([](const WaitPin& pin) { return !pin.waiting; });
                changed_.notify_all();
                return Timeout();
            }
            static_cast<void>(Submit(next));
            lock.lock();
        }
    }

    Result<std::vector<BlockLeasePtr>> Read(std::span<const BlockLocation> locations,
                                          const BlockReadOptions& options) {
        const auto [priority, deadline] = options;
        const bool demand = priority == ReadPriority::demand;
        auto reserved = governor_->TryReserve({MemoryRegion::workspace,
            sizeof(BatchPin) + locations.size() * (sizeof(Item) + sizeof(OutputBlock) + 512)});
        if (const auto* error = std::get_if<Error>(&reserved)) return *error;
        const auto charge = std::get<Reservation>(std::move(reserved));
        const auto pin = std::make_shared<BatchPin>(
            BatchPin{charge, demand ? current_query : nullptr, priority, deadline});
        std::vector<Item> items(locations.size());
        std::vector<BlockLeasePtr> output(locations.size());
        WaitPins pins(locations.size(), WaitPin{pin});
        PendingMap staged;
        std::unique_lock lock(mutex_);
        if (Clock::now() >= deadline) return Timeout();

        // Pin hits before allocating misses, so CLOCK cannot evict this batch's hits.
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (auto hit = cache_.find(locations[i].key); hit != cache_.end()) {
                if (demand) hit->second.touched = true;
                items[i].block = hit->second.block;
                items[i].hit = true;
            }
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
            auto& item = items[i];
            if (!item.hit) {
                const auto& at = locations[i];
                if (auto existing = pending_.find(at.key); existing != pending_.end())
                    item.pending = existing->second;
                else if (auto duplicate = staged.find(at.key); duplicate != staged.end())
                    item.pending = duplicate->second;
                else {
                    auto plan = Prepare(at, priority);
                    if (const auto* error = std::get_if<Error>(&plan)) return *error;
                    item.pending = std::get<std::shared_ptr<Pending>>(std::move(plan));
                    staged.emplace(at.key, item.pending);
                }
                item.block = item.pending->block;
            }
            output[i] = std::make_shared<OutputBlock>(charge, item.block);
        }
        if (Clock::now() >= deadline) return Timeout();
        // Reserve the entire batch before dispatch. Queued misses share a key
        // but only a dispatched read occupies a slot.
        for (auto& item : items) {
            if (item.pending) {
                item.pin = pins.begin();
                item.pending->pins.splice(item.pending->pins.end(), pins, item.pin);
            }
        }
        pending_.merge(staged);
        if (background_calls_) changed_.notify_all();
        struct Cleanup {
            Reader& reader;
            const std::vector<Item>& items;
            std::unique_lock<std::mutex>& lock;
            ~Cleanup() {
                if (!lock.owns_lock()) lock.lock();
                reader.Detach(items);
            }
        } cleanup{*this, items, lock};
        auto result = Dispatch(items, deadline, lock);
        if (auto* error = std::get_if<Error>(&result)) return std::move(*error);

        if (!demand) return output;
        for (const auto& item : items) {
            if (item.hit) ++stats_.cache_hits;
            else ++stats_.cache_misses;
            if (current_query) {
                if (item.hit) ++current_query->cache_hits;
                else ++current_query->cache_misses;
            }
            if (item.block->speculative && !item.block->consumed) {
                item.block->consumed = true;
                ++stats_.prefetch_consumed;
                stats_.prefetch_ready_at_demand += item.hit;
            }
        }
        return output;
    }

public:
    Reader(std::shared_ptr<ResourceGovernor> governor, std::shared_ptr<MeteredReadBackend> backend,
           OptimizedReaderOptions options, Reservation reservation)
        : reservation_(std::move(reservation)), governor_(std::move(governor)),
          backend_(std::move(backend)),
          inline_backend_(dynamic_cast<SynchronousReadBackend*>(backend_.get()) != nullptr),
          options_(options) {}
    ~Reader() override { static_cast<void>(Drain()); }

    Result<std::vector<BlockLeasePtr>> ReadBatch(
        std::span<const BlockLocation> locations, const BlockReadOptions& options) override {
        const bool background = options.priority == ReadPriority::background;
        {
            std::lock_guard lock(mutex_);
            if (closed_) return Closed();
            if (background) {
                if (!options_.scheduling)
                    return Error{ErrorCode::unsupported, "Background read scheduling is disabled"};
                // Maintenance can queue even when demand has borrowed all idle slots.
                if (background_calls_ >= options_.scheduling->max_background_reads)
                    return Exhausted();
                ++background_calls_;
            } else if (calls_ - background_calls_ >= options_.max_inflight_reads) {
                return Exhausted();
            }
            ++calls_;
        }
        Call call{*this, background};
        if (locations.size() > options_.max_inflight_reads ||
            (!background && options.priority != ReadPriority::demand))
            return Error{ErrorCode::invalid_argument, "Invalid block batch"};
        if (locations.empty()) return std::vector<BlockLeasePtr>{};
        for (const auto& at : locations) {
            auto alignment = Validate(at);
            if (const auto* error = std::get_if<Error>(&alignment)) return *error;
        }
        try {
            return Read(locations, options);
        } catch (const std::bad_alloc&) {
            return Exhausted();
        }
    }

    Result<PrefetchDisposition> TryPrefetch(const BlockLocation& at,
                                            Clock::time_point deadline) override {
        auto alignment = Validate(at);
        if (const auto* error = std::get_if<Error>(&alignment)) return *error;
        std::shared_ptr<Pending> pending;
        {
            std::lock_guard lock(mutex_);
            if (closed_) return Closed();
            if (!options_.max_prefetch_reads || Clock::now() >= deadline ||
                ReadFileRetired(*at.file)) return PrefetchDisposition::dropped;
            if (cache_.contains(at.key) || pending_.contains(at.key))
                return PrefetchDisposition::already_present;
            if (pending_.size() > active_reads_ ||
                prefetch_inflight_ >= options_.max_prefetch_reads ||
                active_reads_ >= options_.max_inflight_reads ||
                (current_query && current_query->outstanding_prefetch >=
                                  current_query->prefetch_limit))
                return PrefetchDisposition::dropped;
            if (const auto& scheduling = options_.scheduling;
                scheduling && prefetch_inflight_ + background_inflight_ >=
                    options_.max_inflight_reads - scheduling->demand_reserve)
                return PrefetchDisposition::dropped;
            try {
                auto prepared = Prepare(at, ReadPriority::prefetch);
                if (std::holds_alternative<Error>(prepared)) return PrefetchDisposition::dropped;
                pending = std::get<std::shared_ptr<Pending>>(std::move(prepared));
                pending_.emplace(at.key, pending);
            } catch (const std::bad_alloc&) {
                return PrefetchDisposition::dropped;
            }
            pending->prefetch_query = current_query;
            pending->priority = ReadPriority::prefetch;
            pending->dispatched = true;
            if (current_query) ++current_query->outstanding_prefetch;
            ++active_reads_;
            ++prefetch_inflight_;
            ++calls_;
            NoteDepth();
        }
        Call call{*this};
        return Submit(pending)
            ? PrefetchDisposition::admitted : PrefetchDisposition::dropped;
    }

    Result<Success> Drain() override {
        std::lock_guard drain_lock(drain_mutex_);
        {
            std::unique_lock lock(mutex_);
            closed_ = true;
            changed_.wait(lock, [&] { return calls_ == 0; });
        }
        auto drained = backend_->Drain();
        std::lock_guard lock(mutex_);
        cache_.clear();
        hand_ = cache_.end();
        stats_.cache_resident_bytes = 0;
        return drained;
    }

    BlockReaderStats Stats() const override {
        std::lock_guard lock(mutex_);
        auto result = stats_;
        result.queued_background_reads = QueuedBackground(Clock::now());
        return result;
    }

    Result<Success> Quiesce(Clock::time_point deadline) override {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_until(lock, deadline, [&] { return !calls_ && pending_.empty(); }))
            return Timeout();
        return Success{};
    }

    void Retire(FileId file) override {
        std::lock_guard lock(mutex_);
        for (auto it = cache_.lower_bound({file, 0, 0});
             it != cache_.end() && it->first.file_id == file;)
            it = Remove(it);
    }
};

}  // namespace

QueryReadScope::QueryReadScope(std::shared_ptr<QueryReadState> state)
    : previous_(std::exchange(current_query, std::move(state))) {}
QueryReadScope::~QueryReadScope() { current_query = std::move(previous_); }

Result<std::unique_ptr<OptimizedBlockReader>> CreateOptimizedBlockReader(
    std::shared_ptr<ResourceGovernor> governor, std::shared_ptr<MeteredReadBackend> backend,
    const OptimizedReaderOptions& options) {
    if (!governor || !backend || !options.max_inflight_reads ||
        options.max_prefetch_reads >= options.max_inflight_reads ||
        (options.max_prefetch_reads && (!options.cache_enabled ||
            dynamic_cast<SynchronousReadBackend*>(backend.get()))))
        return Error{ErrorCode::invalid_argument, "Invalid optimized reader options"};
    if (const auto& s = options.scheduling) {
        const auto total = options.max_inflight_reads;
        if (!s->demand_reserve || s->demand_reserve >= total || !s->background_reserve ||
            s->background_reserve > s->max_background_reads ||
            s->max_background_reads > total - s->demand_reserve ||
            !s->demand_burst_before_background ||
            options.max_prefetch_reads > total - s->demand_reserve - s->background_reserve)
            return Error{ErrorCode::invalid_argument, "Invalid fixed read reservations"};
    }
    auto charge = governor->TryReserve({MemoryRegion::metadata, sizeof(Reader) + 256});
    if (const auto* error = std::get_if<Error>(&charge)) return *error;
    try {
        return std::unique_ptr<OptimizedBlockReader>(std::make_unique<Reader>(
            std::move(governor), std::move(backend), options,
            std::get<Reservation>(std::move(charge))));
    } catch (const std::bad_alloc&) {
        return Exhausted();
    }
}

}  // namespace agent_io
