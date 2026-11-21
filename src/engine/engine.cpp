#include "agent_io/engine.hpp"
#include "agent_io/optimized_storage.hpp"

#include "../index/reader.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>

namespace agent_io {
namespace {

Error Failure(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

class StaticLifetime final : public ReadViewLifetime {};

struct QuerySlot {
    std::atomic<std::uint32_t>* count = nullptr;
    ~QuerySlot() { if (count) --*count; }
};

std::uint32_t BlocksPerRow(const NodeRecordLayout& layout, std::uint32_t block_bytes) {
    if (block_bytes == 0) return 1;
    return (std::max)(1u, layout.row_bytes / block_bytes);
}

}  // namespace

struct Engine::Impl {
    EngineOptions options;
    std::filesystem::path directory;
    HANDLE lock = INVALID_HANDLE_VALUE;
    Manifest manifest;
    std::shared_ptr<ResourceGovernor> governor;
    std::shared_ptr<FileRegistry> registry;
    std::shared_ptr<MeteredReadBackend> backend;
    std::shared_ptr<OptimizedBlockReader> reader;
    std::unique_ptr<IndexAdapter> adapter;
    VectorAccess* access = nullptr;
    ReadView view;
    std::shared_ptr<const ReadViewLifetime> lifetime = std::make_shared<StaticLifetime>();
    std::uint64_t unbudgeted_metadata_bytes = 0;
    std::uint64_t manifest_generation = 0;
    std::uint64_t live_count = 0;
    std::atomic<std::uint32_t> active_queries{0};
    mutable std::shared_mutex mutex;
    bool closed = false;

    ~Impl() {
        if (!closed) Release();
    }

    void Release() {
        if (reader) static_cast<void>(reader->Drain());
        adapter.reset();
        reader.reset();
        backend.reset();
        registry.reset();
        governor.reset();
        if (lock != INVALID_HANDLE_VALUE) {
            CloseHandle(lock);
            lock = INVALID_HANDLE_VALUE;
        }
        closed = true;
    }
};

Engine::Engine(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Engine::~Engine() {
    if (impl_ && !impl_->closed) impl_->Release();
}

Result<std::unique_ptr<Engine>> Engine::Open(const std::filesystem::path& directory,
                                             const EngineOptions& options) {
    if (options.memory.total_bytes == 0)
        return Failure(ErrorCode::invalid_argument, "Engine memory budget is empty");
    if (options.block_bytes == 0 || (options.block_bytes & (options.block_bytes - 1)) != 0)
        return Failure(ErrorCode::invalid_argument, "block_bytes must be a power of two");
    if (options.max_queries == 0)
        return Failure(ErrorCode::invalid_argument, "max_queries must be positive");
    if (!options.max_inflight_reads ||
        (options.io_backend != IoBackend::synchronous &&
         options.io_backend != IoBackend::windows_iocp) ||
        (options.cache_policy != CachePolicy::disabled && options.cache_policy != CachePolicy::clock))
        return Failure(ErrorCode::invalid_argument, "Invalid read backend, capacity or cache policy");
    const bool iocp = options.io_backend == IoBackend::windows_iocp;
    const bool cache = options.cache_policy == CachePolicy::clock && options.memory.cache_bytes > 0;
    const auto prefetch = iocp && cache ? options.max_prefetch_reads : 0u;
    if (prefetch && (!options.read_scheduling.demand_reserve ||
        options.read_scheduling.demand_reserve >= options.max_inflight_reads ||
        options.read_scheduling.background_reserve >
            options.max_inflight_reads - options.read_scheduling.demand_reserve ||
        prefetch > options.max_inflight_reads - options.read_scheduling.demand_reserve -
            options.read_scheduling.background_reserve))
        return Failure(ErrorCode::invalid_argument, "Invalid prefetch read reservations");

    std::error_code path_error;
    if (!std::filesystem::is_directory(directory, path_error))
        return Failure(ErrorCode::not_found, "Index directory does not exist: " +
                                             directory.string());

    auto impl = std::make_unique<Impl>();
    impl->options = options;
    impl->directory = directory;

    // LOCK is an exclusive handle, not a file-existence check.
    const auto lock_path = directory / "LOCK";
    impl->lock = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (impl->lock == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        Error error{ErrorCode::io_error,
                    "Cannot lock the index directory; another instance may hold it"};
        error.system_error = static_cast<int>(code);
        return error;
    }

    auto governor = CreateResourceGovernor(options.memory, options.rebuild_memory);
    if (const auto* error = std::get_if<Error>(&governor)) return *error;
    impl->governor = std::get<std::shared_ptr<ResourceGovernor>>(governor);

    SynchronousStorageOptions storage;
    storage.direct_reads = options.direct_reads;
    storage.max_registered_files = (std::max)(std::uint32_t{64},
        options.resources.max_disk_segments * 2u);
    storage.max_inflight_reads = options.max_inflight_reads;
    storage.overlapped = iocp;
    if (iocp) {
        auto backend = CreateIocpReadBackend(impl->governor, storage.max_inflight_reads);
        if (const auto* error = std::get_if<Error>(&backend)) return *error;
        impl->backend = std::get<std::unique_ptr<MeteredReadBackend>>(std::move(backend));
    } else {
        auto backend = CreateSynchronousReadBackend(impl->governor, storage);
        if (const auto* error = std::get_if<Error>(&backend)) return *error;
        impl->backend = std::get<std::unique_ptr<SynchronousReadBackend>>(std::move(backend));
    }
    auto reader = CreateOptimizedBlockReader(impl->governor, impl->backend,
        {options.max_inflight_reads, prefetch, cache});
    if (const auto* error = std::get_if<Error>(&reader)) return *error;
    impl->reader = std::get<std::unique_ptr<OptimizedBlockReader>>(std::move(reader));

    auto registry = CreateSynchronousFileRegistry(impl->governor, storage,
        [weak = std::weak_ptr(impl->reader)](FileId file) {
            if (auto held = weak.lock()) held->Retire(file);
        });
    if (const auto* error = std::get_if<Error>(&registry)) return *error;
    impl->registry = std::shared_ptr<FileRegistry>(
        std::get<std::unique_ptr<FileRegistry>>(std::move(registry)));

    auto opened = OpenIndexDirectory(directory, impl->reader, impl->registry,
                                     options.block_bytes, options.max_inflight_reads);
    if (const auto* error = std::get_if<Error>(&opened)) return *error;
    auto index = std::get<OpenedIndex>(std::move(opened));
    impl->manifest = index.manifest;
    impl->adapter = std::move(index.adapter);
    impl->unbudgeted_metadata_bytes = index.unbudgeted_metadata_bytes;
    impl->manifest_generation = impl->manifest.generation;
    impl->live_count = impl->manifest.live_count;
    impl->access = dynamic_cast<VectorAccess*>(impl->adapter.get());
    if (impl->access == nullptr)
        return Failure(ErrorCode::unsupported, "Adapter does not expose point reads");

    impl->view.instance_id = impl->manifest.instance_id;
    impl->view.view_generation = 1;
    impl->view.snapshot_sequence = 0;
    impl->view.manifest_generation = impl->manifest.generation;
    impl->view.live_count = impl->manifest.live_count;
    if (impl->manifest.segment_count > kMaxSegmentRefs)
        return Failure(ErrorCode::corruption, "Manifest exceeds the view segment capacity");
    for (std::uint32_t index_entry = 0; index_entry < impl->manifest.segment_count;
         ++index_entry) {
        const auto& entry = impl->manifest.segments[index_entry];
        SegmentDescriptor& descriptor = impl->view.segments[index_entry];
        descriptor.id = entry.id;
        descriptor.generation = entry.generation;
        descriptor.kind = entry.kind;
        descriptor.covered_sequence = entry.covered_sequence;
        descriptor.node_count = entry.node_count;
        descriptor.file_bytes = entry.file_bytes;
        descriptor.block_bytes = impl->manifest.block_bytes;
        descriptor.layout = impl->manifest.layout;
        descriptor.has_version_directory = true;
        descriptor.content_sha256 = entry.content_sha256;
    }
    impl->view.segment_count = impl->manifest.segment_count;
    impl->view.delta_count = 0;
    return std::unique_ptr<Engine>(new Engine(std::move(impl)));
}

Result<SearchResult> Engine::Search(std::span<const float> query,
                                    const SearchOptions& options) {
    if (!impl_) return Failure(ErrorCode::closed, "Engine is closed");
    Impl& impl = *impl_;
    if (query.size() != impl.manifest.dimension)
        return Failure(ErrorCode::invalid_argument, "Query dimension does not match the index");
    if (options.k == 0 || options.k > options.candidate_limit)
        return Failure(ErrorCode::invalid_argument, "k must be within the candidate limit");
    if (options.candidate_limit == 0 || options.candidate_limit > 4096)
        return Failure(ErrorCode::invalid_argument, "candidate_limit is out of range");
    if (options.timeout.count() <= 0)
        return Failure(ErrorCode::invalid_argument, "timeout must be positive");
    if (options.prefetch_blocks > impl.options.max_prefetch_reads)
        return Failure(ErrorCode::invalid_argument, "Query prefetch exceeds the engine cap");

    std::shared_lock guard(impl.mutex);
    if (impl.closed) return Failure(ErrorCode::closed, "Engine is closed");
    auto slot = std::make_shared<QuerySlot>();
    auto active = impl.active_queries.load();
    do {
        if (active >= impl.options.max_queries)
            return Failure(ErrorCode::resource_exhausted, "Query admission capacity exhausted");
    } while (!impl.active_queries.compare_exchange_weak(active, active + 1));
    slot->count = &impl.active_queries;
    ReadViewLease lease;
    lease.view = impl.view;
    lease.lifetime = impl.lifetime;
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + options.timeout;

    SearchResult result;
    result.snapshot_sequence = lease.view.snapshot_sequence;
    result.view_generation = lease.view.view_generation;
    if (impl.live_count == 0) {
        result.metrics.elapsed_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started).count());
        return result;
    }

    const std::uint32_t query_share = (std::max)(1u, impl.options.max_queries);
    const std::uint64_t per_query_workspace = impl.options.memory.workspace_bytes / query_share;
    CandidateBudget budget;
    budget.total_limit = options.candidate_limit;
    budget.per_segment_limit = (std::min)(std::uint32_t{64}, budget.total_limit);
    budget.delta_row_limit = impl.options.resources.max_retained_delta_vectors;
    budget.workspace_bytes = per_query_workspace;

    const auto layout = impl.manifest.layout;
    const std::uint32_t blocks_per_row = BlocksPerRow(layout, impl.manifest.block_bytes);
    const std::uint64_t visited_cap =
        static_cast<std::uint64_t>(budget.total_limit) * layout.max_degree;
    ScratchRequest scratch;
    scratch.dimension = impl.manifest.dimension;
    scratch.budget = budget;
    scratch.delta_slots = 0;
    scratch.segment_count = lease.view.segment_count;
    scratch.max_degree = layout.max_degree;
    scratch.copy_buffer_blocks = layout.max_degree + blocks_per_row;
    scratch.block_bytes = impl.manifest.block_bytes;
    // The project-native adapter stores raw vectors: no compressed codes are
    // allocated, which is a proof by construction rather than an estimate.
    scratch.pq_scratch_bytes = std::uint64_t{0};
    scratch.navigation_scratch_bytes = visited_cap * 32 + layout.max_degree * 16 +
        static_cast<std::uint64_t>(budget.total_limit) * 32 + 4096;
    const auto plan = ComputeScratchPlan(scratch);
    if (const auto* error = std::get_if<Error>(&plan)) return *error;

    auto reservation = impl.governor->TryReserve(
        MemoryCharge{MemoryRegion::workspace,
            std::get<ScratchPlan>(plan).total_bytes + sizeof(QueryReadState) + 256});
    if (const auto* error = std::get_if<Error>(&reservation)) return *error;
    auto query_reads = std::make_shared<QueryReadState>();
    query_reads->admission = slot;
    query_reads->workspace = std::get<std::shared_ptr<const BudgetReservation>>(std::move(reservation));
    if (impl.options.io_backend == IoBackend::windows_iocp &&
        impl.options.cache_policy == CachePolicy::clock && impl.options.memory.cache_bytes)
        query_reads->prefetch_limit = options.prefetch_blocks;
    QueryReadScope read_scope(query_reads);

    const ExpansionRequest request{query, lease, budget, options.k, deadline,
                                   query_reads->prefetch_limit};
    auto expanded = impl.adapter->Expand(request);
    if (const auto* error = std::get_if<Error>(&expanded)) return *error;
    auto expansion = std::get<ExpansionResult>(std::move(expanded));

    const std::span<const Candidate> candidate_view(expansion.candidates);
    const RefinementRequest refine_request{candidate_view, query, lease, budget, deadline};
    auto refined = impl.adapter->Refine(refine_request);
    if (const auto* error = std::get_if<Error>(&refined)) return *error;
    const auto& candidates = std::get<std::vector<Candidate>>(refined);
    result.neighbors.reserve((std::min)(static_cast<std::size_t>(options.k), candidates.size()));
    for (const auto& candidate : candidates) {
        if (result.neighbors.size() >= options.k) break;
        Neighbor neighbour;
        neighbour.id = candidate.id;
        neighbour.version = candidate.version;
        neighbour.squared_distance = candidate.exact_distance.value_or(0.0);
        result.neighbors.push_back(neighbour);
    }
    result.metrics.cache_hits = query_reads->cache_hits;
    result.metrics.cache_misses = query_reads->cache_misses;
    result.metrics.logical_block_demands = result.metrics.cache_hits + result.metrics.cache_misses;
    result.metrics.delta_records_scanned = 0;
    result.metrics.candidate_limit_reached = expansion.candidate_limit_reached;
    result.metrics.elapsed_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count());
    return result;
}

Result<StoredVector> Engine::Get(VectorId id) {
    if (!impl_) return Failure(ErrorCode::closed, "Engine is closed");
    Impl& impl = *impl_;
    if (id == 0) return Failure(ErrorCode::invalid_argument, "Logical ID must be non-zero");
    std::shared_lock guard(impl.mutex);
    if (impl.closed) return Failure(ErrorCode::closed, "Engine is closed");
    const auto& view = impl.view;
    std::optional<VersionDirectoryEntry> best;
    for (std::uint32_t index = 0; index < view.segment_count; ++index) {
        const auto found = impl.access->FindId(view.segments[index].id, id);
        if (const auto* error = std::get_if<Error>(&found)) return *error;
        const auto& entry = std::get<std::optional<VersionDirectoryEntry>>(found);
        if (!entry.has_value()) continue;
        if (entry->version > view.snapshot_sequence) continue;
        if (!best.has_value() || best->version < entry->version) best = entry;
    }
    if (!best.has_value()) return Failure(ErrorCode::not_found, "Logical ID is not visible");
    for (std::uint32_t index = 0; index < view.segment_count; ++index) {
        const auto stored = impl.access->ReadVector(view.segments[index].id, best->node,
                                                    view.snapshot_sequence);
        if (const auto* error = std::get_if<Error>(&stored)) {
            if (error->code == ErrorCode::not_found) continue;
            return *error;
        }
        const auto& vector = std::get<StoredVector>(stored);
        if (vector.id == id) return vector;
    }
    return Failure(ErrorCode::not_found, "Logical ID could not be read back");
}

Result<WriteReceipt> Engine::Insert(VectorId, std::span<const float>, const WriteOptions&) {
    return Failure(ErrorCode::unsupported,
                   "The static baseline has no write path; WAL and delta stores arrive with the "
                   "dynamic stage");
}

Result<WriteReceipt> Engine::Upsert(VectorId, std::span<const float>, const WriteOptions&) {
    return Failure(ErrorCode::unsupported,
                   "The static baseline has no write path; WAL and delta stores arrive with the "
                   "dynamic stage");
}

Result<CheckpointReceipt> Engine::Checkpoint(std::chrono::milliseconds) {
    return Failure(ErrorCode::unsupported,
                   "The static baseline has no checkpoint path; there is nothing to flush");
}

Result<EngineStats> Engine::Stats() const {
    if (!impl_) return Failure(ErrorCode::closed, "Engine is closed");
    const Impl& impl = *impl_;
    std::shared_lock guard(impl.mutex);
    EngineStats stats;
    stats.published_sequence = 0;
    stats.checkpoint_sequence = impl.manifest.checkpoint_sequence;
    stats.view_generation = impl.view.view_generation;
    stats.live_vectors = impl.live_count;
    if (impl.governor) stats.memory_accounted_bytes = MemoryAccountedBytes(*impl.governor);
    stats.delta_resident_bytes = 0;
    stats.retired_pinned_bytes = 0;
    if (impl.backend) {
        const auto counters = impl.backend->Stats();
        stats.io_reads_submitted = counters.io_reads_submitted;
        stats.io_read_bytes_completed = counters.io_read_bytes_completed;
        stats.io_read_errors = counters.io_read_errors;
    }
    if (impl.reader) {
        const auto reads = impl.reader->Stats();
        stats.cache_resident_bytes = reads.cache_resident_bytes;
        stats.cache_hits = reads.cache_hits;
        stats.cache_misses = reads.cache_misses;
        stats.prefetch_submitted = reads.prefetch_submitted;
        stats.prefetch_consumed = reads.prefetch_consumed;
        stats.prefetch_ready_at_demand = reads.prefetch_ready_at_demand;
        stats.peak_inflight_reads = reads.peak_inflight_reads;
        stats.peak_prefetch_reads = reads.peak_prefetch_reads;
    }
    stats.wal_bytes_written = 0;
    stats.checkpoint_bytes_written = 0;
    stats.merge_bytes_written = 0;
    stats.logical_update_bytes = 0;
    stats.pending_write_count = 0;
    stats.merge_backlog_bytes = 0;
    return stats;
}

Result<Success> Engine::QuiesceReads(std::chrono::steady_clock::time_point deadline) {
    if (!impl_) return Failure(ErrorCode::closed, "Engine is closed");
    std::shared_lock guard(impl_->mutex);
    if (impl_->closed) return Failure(ErrorCode::closed, "Engine is closed");
    return impl_->reader->Quiesce(deadline);
}

Result<Success> Engine::Close() {
    if (!impl_) return Failure(ErrorCode::closed, "Engine is closed");
    std::unique_lock guard(impl_->mutex);
    if (impl_->closed) return Failure(ErrorCode::closed, "Engine is already closed");
    impl_->Release();
    return Success{};
}

}  // namespace agent_io
