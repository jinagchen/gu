#include "agent_io/engine.hpp"
#include "agent_io/optimized_storage.hpp"
#include "agent_io/storage.hpp"

#include "../index/delta.hpp"
#include "../index/digest.hpp"
#include "../index/publish.hpp"
#include "../index/reader.hpp"
#include "../index/recovery.hpp"
#include "../index/segment_writer.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

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

double SquaredL2(const float* left, const float* right, std::uint32_t dimension) {
    double total = 0.0;
    for (std::uint32_t index = 0; index < dimension; ++index) {
        const double delta = static_cast<double>(left[index]) - static_cast<double>(right[index]);
        total += delta * delta;
    }
    return total;
}

bool AllFinite(std::span<const float> values) {
    for (const auto value : values) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        if ((bits & 0x7f800000u) == 0x7f800000u) return false;
    }
    return true;
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

    // --- stage 6: durable writes ---------------------------------------
    std::filesystem::path wal_directory;
    std::unique_ptr<DeltaLayer> delta;
    std::unique_ptr<WalWriter> wal;
    // Serialises publication: a write reserves a slot, appends, syncs, and only
    // then moves the prefix. One coordinator keeps the prefix and the WAL in
    // the same order without a second protocol.
    std::mutex publication_mutex;
    Sequence published_sequence = 0;
    std::atomic<std::uint32_t> pending_writes{0};
    std::uint64_t wal_file_bytes = 0;
    std::uint64_t wal_bytes_written = 0;
    std::uint64_t logical_update_bytes = 0;
    std::uint64_t delta_resident_bytes = 0;
    std::uint64_t frame_bytes = 0;
    // Set when a sync failed after bytes may have reached the file. The engine
    // then refuses further writes rather than guess what the file contains.
    bool recovery_required = false;
    std::string recovery_message;

    // Recovery facts, kept for Stats and for the run artifacts.
    std::uint64_t replayed_records = 0;
    std::uint64_t discarded_tail_bytes = 0;
    std::uint64_t wal_files_scanned = 0;

    // --- stage 7: checkpoint, compaction and reclamation ---------------
    // One checkpoint or compaction at a time; the writer path is serialised by
    // publication_mutex, and this lock keeps the two maintenance shapes apart.
    std::mutex maintenance_mutex;
    std::mutex maintenance_wait_mutex;
    std::condition_variable_any maintenance_cv;
    std::thread maintenance;
    std::atomic<bool> stopping{false};
    std::atomic<bool> shut_down{false};
    std::uint64_t checkpoint_bytes_written = 0;
    std::uint64_t merge_bytes_written = 0;
    std::uint64_t checkpoints_completed = 0;
    // Visible IDs that no segment holds yet. The manifest may only count what
    // is stored, so this is what a publish adds to manifest.live_count.
    std::uint32_t pending_new_ids = 0;
    std::uint64_t compactions_completed = 0;
    std::uint64_t wal_files_reclaimed = 0;
    std::uint64_t segments_reclaimed = 0;
    std::uint64_t maintenance_errors = 0;
    std::string last_maintenance_error;
    // Superseded segment directories. A generation is published before its
    // predecessor is dropped, so a deletion failure only costs disk, never
    // correctness; the queue is retried on every maintenance tick.
    std::vector<std::filesystem::path> recycle_queue;

    ~Impl() {
        RequestStop();
        Shutdown();
    }

    void RequestStop() {
        stopping.store(true);
        maintenance_cv.notify_all();
    }

    // A checkpoint or merge that is already staging would otherwise publish
    // into a closed engine and leave its directory behind, so teardown waits
    // for the maintenance thread to come back before anything is released.
    void JoinMaintenance() {
        if (maintenance.joinable()) maintenance.join();
    }

    void Shutdown() {
        JoinMaintenance();
        if (shut_down.exchange(true)) return;
        if (wal) static_cast<void>(wal->Close());
        wal.reset();
        delta.reset();
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

    // Rebuilds the snapshot the readers see. Called under the publication lock
    // so a captured view never mixes a new prefix with an old delta pin.
    void PublishView() {
        view.snapshot_sequence = published_sequence;
        view.live_count = live_count;
        view.delta_count = 0;
        if (delta && delta->CommittedCount() > 0 && view.delta_count < kMaxDeltaPins) {
            view.deltas[view.delta_count] = delta->CapturePin();
            ++view.delta_count;
        }
    }

    // True when the ID already has a row in a published segment, i.e. the
    // manifest's live count already includes it. Recovery needs this to tell a
    // genuinely new ID from an overwrite of a vector that was there all along.
    Result<bool> ExistsInBase(VectorId id) {
        for (std::uint32_t index = 0; index < view.segment_count; ++index) {
            const auto found = access->FindId(view.segments[index].id, id);
            if (const auto* error = std::get_if<Error>(&found)) return *error;
            if (std::get<std::optional<VersionDirectoryEntry>>(found).has_value()) return true;
        }
        return false;
    }

    // Highest version of `id` visible at `snapshot`, across the delta and every
    // segment. Existence questions must consult both, otherwise a fresh insert
    // would look absent to a duplicate check while a reader already sees it.
    Result<std::optional<Sequence>> VisibleVersion(VectorId id, Sequence snapshot) {
        std::optional<Sequence> best;
        if (delta) {
            const auto committed = delta->CommittedCount();
            for (std::uint32_t index = 0; index < committed; ++index) {
                if (delta->IdAt(index) != id) continue;
                const auto version = delta->VersionAt(index);
                if (version > snapshot) continue;
                if (!best.has_value() || *best < version) best = version;
            }
        }
        for (std::uint32_t index = 0; index < view.segment_count; ++index) {
            const auto found = access->FindId(view.segments[index].id, id);
            if (const auto* error = std::get_if<Error>(&found)) return *error;
            const auto& entry = std::get<std::optional<VersionDirectoryEntry>>(found);
            if (!entry.has_value() || entry->version > snapshot) continue;
            if (!best.has_value() || *best < entry->version) best = entry->version;
        }
        return best;
    }

    // The single write path. Insert and Upsert differ only in whether an
    // existing ID is a conflict, so the durability protocol lives here once.
    Result<WriteReceipt> ApplyWrite(WalOperation operation, VectorId id,
                                    std::span<const float> values,
                                    const WriteOptions& write_options);

    // --- stage 7: checkpoint, compaction, reclamation -------------------
    bool MaintenanceEnabled() const noexcept {
        return options.rebuild_memory.has_value() && options.resources.max_disk_segments > 0;
    }
    // Flushes the published delta prefix into a new segment and switches the
    // generation. Safe to call concurrently with readers; writers wait.
    Result<CheckpointReceipt> RunCheckpoint(std::chrono::milliseconds timeout);
    // Rewrites every segment into one. Only runs when the segment count is over
    // the declared budget, and only through the same publish path.
    Result<Success> RunCompaction(std::chrono::milliseconds timeout);
    Result<Success> RotateWalAfter(Sequence sequence);
    Result<Success> AdoptPublished(OpenedIndex&& opened);
    // Reads every row of every segment, keeping the newest version per ID.
    Result<std::vector<SegmentRecord>> ReadAllRows(Sequence snapshot,
                                                   std::uint64_t byte_budget);
    void RecycleWalUpTo(Sequence target);
    void RecycleDue(std::size_t limit);
    void MaintenanceLoop();
};


Engine::Engine(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Engine::~Engine() {
    if (impl_) {
        impl_->RequestStop();
        impl_->Shutdown();
    }
}

Result<Success> Engine::Impl::RotateWalAfter(Sequence sequence) {
    if (!wal) return Success{};
    // An empty file has no synced group to close, and the writer refuses to
    // rotate one. Nothing to do: the next write simply continues in it.
    if (wal_file_bytes <= wal_detail::kWalHeaderBytes) return Success{};
    WalFileHeader header{};
    header.instance_id = manifest.instance_id;
    header.start_sequence = sequence + 1;
    auto rotated = wal->Rotate(header);
    if (const auto* error = std::get_if<Error>(&rotated)) {
        // The writer may already have created the next file; further writes
        // would guess which file the engine is on.
        recovery_required = true;
        recovery_message = error->message;
        return *error;
    }
    wal_file_bytes = wal_detail::kWalHeaderBytes;
    return Success{};
}

Result<std::vector<SegmentRecord>> Engine::Impl::ReadAllRows(Sequence snapshot,
                                                             std::uint64_t byte_budget) {
    std::vector<SegmentRecord> rows;
    std::unordered_map<VectorId, std::size_t> position;
    std::uint64_t bytes = 0;
    for (std::uint32_t segment_index = 0; segment_index < view.segment_count; ++segment_index) {
        const auto segment = view.segments[segment_index].id;
        const auto nodes = view.segments[segment_index].node_count;
        for (std::uint64_t node = 0; node < nodes; ++node) {
            auto stored = access->ReadVector(segment, static_cast<NativeNodeId>(node), snapshot);
            if (const auto* error = std::get_if<Error>(&stored)) {
                if (error->code == ErrorCode::not_found) continue;
                return *error;
            }
            auto vector = std::get<StoredVector>(std::move(stored));
            bytes += vector.values.size() * sizeof(float) + sizeof(SegmentRecord);
            if (byte_budget != 0 && bytes > byte_budget)
                return Failure(ErrorCode::resource_exhausted,
                               "Compaction needs more workspace than rebuild_memory allows");
            const auto found = position.find(vector.id);
            if (found == position.end()) {
                position.emplace(vector.id, rows.size());
                SegmentRecord record;
                record.id = vector.id;
                record.version = vector.version;
                record.values = std::move(vector.values);
                rows.push_back(std::move(record));
            } else if (rows[found->second].version < vector.version) {
                // A newer version in another segment wins; the version rule is
                // the same one queries use, so a merge cannot change results.
                rows[found->second].version = vector.version;
                rows[found->second].values = std::move(vector.values);
            }
        }
    }
    return rows;
}

Result<Success> Engine::Impl::AdoptPublished(OpenedIndex&& opened) {
    if (opened.manifest.segment_count > kMaxSegmentRefs)
        return Failure(ErrorCode::corruption, "Published manifest exceeds the view capacity");
    adapter = std::move(opened.adapter);
    access = dynamic_cast<VectorAccess*>(adapter.get());
    if (access == nullptr)
        return Failure(ErrorCode::unsupported, "Adapter does not expose point reads");
    manifest = opened.manifest;
    manifest_generation = manifest.generation;
    unbudgeted_metadata_bytes = opened.unbudgeted_metadata_bytes;
    view.instance_id = manifest.instance_id;
    view.manifest_generation = manifest.generation;
    view.view_generation = view.view_generation + 1;
    view.snapshot_sequence = published_sequence;
    view.live_count = live_count;
    view.segment_count = manifest.segment_count;
    for (std::uint32_t index = 0; index < manifest.segment_count; ++index) {
        const auto& entry = manifest.segments[index];
        SegmentDescriptor& descriptor = view.segments[index];
        descriptor.id = entry.id;
        descriptor.generation = entry.generation;
        descriptor.kind = entry.kind;
        descriptor.covered_sequence = entry.covered_sequence;
        descriptor.node_count = entry.node_count;
        descriptor.file_bytes = entry.file_bytes;
        descriptor.block_bytes = manifest.block_bytes;
        descriptor.layout = manifest.layout;
        descriptor.has_version_directory = true;
        descriptor.content_sha256 = entry.content_sha256;
    }
    return Success{};
}

void Engine::Impl::RecycleWalUpTo(Sequence target) {
    std::uint64_t failed = 0;
    auto removed = RecycleWalFiles(wal_directory, manifest.instance_id, target, &failed);
    if (const auto* error = std::get_if<Error>(&removed)) {
        last_maintenance_error = error->message;
        ++maintenance_errors;
        return;
    }
    wal_files_reclaimed += std::get<std::uint64_t>(removed);
}

void Engine::Impl::RecycleDue(std::size_t limit) {
    std::size_t done = 0;
    for (auto item = recycle_queue.begin();
         item != recycle_queue.end() && done < limit;) {
        std::error_code error;
        std::filesystem::remove_all(*item, error);
        if (error) {
            // A reader may still hold a handle. Keeping the directory costs
            // disk and nothing else, so retry on the next tick instead of
            // forcing the issue.
            ++item;
            continue;
        }
        item = recycle_queue.erase(item);
        ++done;
        ++segments_reclaimed;
    }
}

Result<CheckpointReceipt> Engine::Impl::RunCheckpoint(std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0)
        return Failure(ErrorCode::invalid_argument, "Checkpoint timeout must be positive");
    std::lock_guard<std::mutex> maintenance_guard(maintenance_mutex);

    std::vector<SegmentRecord> records;
    Sequence target = 0;
    std::uint32_t frozen = 0;
    std::uint32_t captured_new = 0;
    std::uint32_t dimension = 0;
    std::uint32_t block_bytes = 0;
    NodeRecordLayout layout;
    std::string builder_name;
    std::string builder_parameters;
    {
        std::unique_lock<std::shared_mutex> guard(mutex);
        if (closed) return Failure(ErrorCode::closed, "Engine is closed");
        if (recovery_required) return Error{ErrorCode::recovery_required, recovery_message};
        if (!delta)
            return Failure(ErrorCode::unsupported,
                           "This index has no delta budget; there is nothing to checkpoint");
        target = published_sequence;
        frozen = delta->CommittedCount();
        captured_new = pending_new_ids;
        if (frozen == 0) {
            CheckpointReceipt receipt;
            receipt.target_sequence = target;
            receipt.checkpoint_sequence = manifest.checkpoint_sequence;
            receipt.manifest_generation = manifest.generation;
            return receipt;
        }
        dimension = manifest.dimension;
        block_bytes = manifest.block_bytes;
        layout = manifest.layout;
        builder_name = std::string(ManifestTextView(manifest.adapter.name));
        builder_parameters = std::string(ManifestTextView(manifest.adapter.parameters));
        // One directory row per ID: an older version is already hidden by the
        // version rule and would only make the directory ambiguous.
        std::unordered_map<VectorId, std::uint32_t> newest;
        newest.reserve(frozen);
        for (std::uint32_t index = 0; index < frozen; ++index) {
            const auto id = delta->IdAt(index);
            const auto found = newest.find(id);
            if (found == newest.end())
                newest.emplace(id, index);
            else if (delta->VersionAt(found->second) < delta->VersionAt(index))
                found->second = index;
        }
        records.reserve(newest.size());
        for (const auto& item : newest) {
            SegmentRecord record;
            record.id = item.first;
            record.version = delta->VersionAt(item.second);
            const auto values = delta->ValuesAt(item.second);
            record.values.assign(values.begin(), values.end());
            records.push_back(std::move(record));
        }
        // Close the WAL epoch here: everything up to the target is now in a
        // file nothing will append to, so the publish below may reclaim it
        // without touching a byte written afterwards.
        const auto rotated = RotateWalAfter(target);
        if (const auto* error = std::get_if<Error>(&rotated)) return *error;
    }

    // The segment is built outside the view lock: queries keep running against
    // the old generation while it is staged. Only the switch is exclusive.
    const auto staging = directory / ("checkpoint-" + FormatUuid(NewInstanceId()));
    SegmentWriteRequest request;
    request.staging_directory = staging;
    request.dimension = dimension;
    request.block_bytes = block_bytes;
    request.layout = layout;
    request.records = std::move(records);
    request.builder = builder_name;
    request.builder_version = "1.0.0";
    request.parameters = builder_parameters;
    auto written = WriteSegment(request);
    if (const auto* error = std::get_if<Error>(&written)) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        return *error;
    }
    const auto artifact = std::get<SegmentArtifact>(std::move(written));

    std::error_code path_error;
    const auto segments_directory = directory / "segments";
    std::filesystem::create_directories(segments_directory, path_error);
    if (path_error) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        ++maintenance_errors;
        return Failure(ErrorCode::io_error, "Cannot create the segments directory");
    }
    const auto segment_directory = segments_directory / FormatUuid(artifact.uuid);
    if (!MoveFileExW(staging.c_str(), segment_directory.c_str(), MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        ++maintenance_errors;
        return Error{ErrorCode::io_error, "Cannot move the segment into place",
                       static_cast<int>(GetLastError())};
    }

    std::unique_lock<std::shared_mutex> guard(mutex);
    std::lock_guard<std::mutex> publication(publication_mutex);
    if (closed) {
        recycle_queue.push_back(segment_directory);
        return Failure(ErrorCode::closed, "Engine closed while the checkpoint was staged");
    }
    if (manifest.segment_count >= kMaxManifestSegments) {
        recycle_queue.push_back(segment_directory);
        return Failure(ErrorCode::resource_exhausted,
                       "The manifest segment budget is full; compaction is required");
    }
    Manifest next = manifest;
    next.generation = manifest.generation + 1;
    next.checkpoint_sequence = target;
    next.wal_replay_after = target;
    // live_count counts what segments store. Writes that arrived while this
    // segment was being built are still only in the delta, so they are not part
    // of it yet -- the counter carries them to the next publish.
    const std::uint32_t stored_after = static_cast<std::uint32_t>(
        manifest.live_count + captured_new);
    next.live_count = stored_after;
    SegmentId highest = 0;
    for (std::uint32_t index = 0; index < next.segment_count; ++index)
        highest = (std::max)(highest, next.segments[index].id);
    auto& entry = next.segments[next.segment_count];
    entry.id = highest + 1;
    entry.uuid = artifact.uuid;
    entry.generation = next.generation;
    entry.kind = SegmentKind::flushed_delta;
    entry.covered_sequence = target;
    entry.node_count = artifact.node_count;
    entry.file_bytes = artifact.graph_bytes;
    entry.content_sha256 = artifact.graph_sha256;
    const auto metadata_text = SetManifestText(
        entry.metadata.path, "segments/" + FormatUuid(artifact.uuid) + "/segment.json");
    if (const auto* error = std::get_if<Error>(&metadata_text)) {
        recycle_queue.push_back(segment_directory);
        return *error;
    }
    entry.metadata.logical_bytes = artifact.metadata_bytes;
    entry.metadata.physical_bytes = artifact.metadata_bytes;
    entry.metadata.sha256 = artifact.metadata_sha256;
    next.segment_count = manifest.segment_count + 1;

    const auto published = PublishManifest(directory, next);
    if (const auto* error = std::get_if<Error>(&published)) {
        recycle_queue.push_back(segment_directory);
        ++maintenance_errors;
        return *error;
    }
    auto opened = OpenIndexDirectory(directory, reader, registry, options.block_bytes,
                                     options.max_inflight_reads);
    if (const auto* error = std::get_if<Error>(&opened)) {
        // CURRENT already names the new generation. Serving the old adapter
        // would hand out a view that disagrees with the disk, so stop writing
        // and let a restart pick up the published state.
        recovery_required = true;
        recovery_message = "Published generation " + std::to_string(next.generation) +
                           " but could not reopen it: " + error->message;
        ++maintenance_errors;
        last_maintenance_error = recovery_message;
        return Error{ErrorCode::recovery_required, recovery_message};
    }
    const auto adopted = AdoptPublished(std::get<OpenedIndex>(std::move(opened)));
    if (const auto* error = std::get_if<Error>(&adopted)) return *error;
    static_cast<void>(delta->RetirePrefix(frozen));
    pending_new_ids -= pending_new_ids > captured_new ? captured_new : pending_new_ids;
    PublishView();
    checkpoint_bytes_written += artifact.staged_bytes;
    ++checkpoints_completed;
    CheckpointReceipt receipt;
    receipt.target_sequence = target;
    receipt.checkpoint_sequence = manifest.checkpoint_sequence;
    receipt.manifest_generation = manifest.generation;
    guard.unlock();
    RecycleWalUpTo(target);
    RecycleDue(4);
    return receipt;
}

Result<Success> Engine::Impl::RunCompaction(std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0)
        return Failure(ErrorCode::invalid_argument, "Compaction timeout must be positive");

    // A merge rewrites segments only, so anything still in the delta is flushed
    // first. Claiming a checkpoint sequence for a record no segment holds would
    // let the next restart skip it -- the one way a merge could lose a write.
    bool pending = false;
    {
        std::shared_lock<std::shared_mutex> guard(mutex);
        if (closed) return Failure(ErrorCode::closed, "Engine is closed");
        if (recovery_required) return Error{ErrorCode::recovery_required, recovery_message};
        pending = delta && delta->CommittedCount() > 0;
    }
    if (pending) {
        const auto flushed = RunCheckpoint(timeout);
        if (const auto* error = std::get_if<Error>(&flushed)) return *error;
    }

    std::lock_guard<std::mutex> maintenance_guard(maintenance_mutex);
    Sequence materialized = 0;
    Sequence published_at_copy = 0;
    std::uint32_t retire_count = 0;
    std::uint32_t dimension = 0;
    std::uint32_t block_bytes = 0;
    NodeRecordLayout layout;
    std::string builder_name;
    std::string builder_parameters;
    std::vector<SegmentRecord> rows;
    std::vector<std::filesystem::path> superseded;
    std::vector<FileId> superseded_files;
    {
        std::unique_lock<std::shared_mutex> guard(mutex);
        if (closed) return Failure(ErrorCode::closed, "Engine is closed");
        if (recovery_required) return Error{ErrorCode::recovery_required, recovery_message};
        if (manifest.segment_count < 2) return Success{};
        // A merge rewrites everything it reads, so a set of nearly empty
        // segments is not worth the I/O yet; the next tick tries again.
        if (options.merge_min_bytes != 0) {
            std::uint64_t live_bytes = 0;
            for (std::uint32_t index = 0; index < manifest.segment_count; ++index)
                live_bytes += manifest.segments[index].file_bytes;
            if (live_bytes < options.merge_min_bytes) return Success{};
        }
        published_at_copy = published_sequence;
        dimension = manifest.dimension;
        block_bytes = manifest.block_bytes;
        layout = manifest.layout;
        builder_name = std::string(ManifestTextView(manifest.adapter.name));
        builder_parameters = std::string(ManifestTextView(manifest.adapter.parameters));
        const std::uint64_t budget = options.rebuild_memory.has_value()
                                         ? options.rebuild_memory->workspace_bytes
                                         : 0;
        auto read = ReadAllRows(published_at_copy, budget);
        if (const auto* error = std::get_if<Error>(&read)) return *error;
        rows = std::get<std::vector<SegmentRecord>>(std::move(read));
        if (rows.empty())
            return Failure(ErrorCode::not_found, "Compaction found no rows to merge");
        // The merged segment covers exactly what its rows carry. Anything newer
        // stays in the delta and in the WAL, and the checkpoint sequence must
        // not claim it.
        for (const auto& row : rows) materialized = (std::max)(materialized, row.version);
        if (delta) {
            while (retire_count < delta->CommittedCount() &&
                   delta->VersionAt(retire_count) <= materialized)
                ++retire_count;
        }
        // Rotation is pinned to the published sequence: the writer expects the
        // next file to start where the next write goes, not at the merge
        // boundary.
        const auto rotated = RotateWalAfter(published_at_copy);
        if (const auto* error = std::get_if<Error>(&rotated)) return *error;
        for (std::uint32_t index = 0; index < manifest.segment_count; ++index)
            superseded.push_back(directory / "segments" /
                                 FormatUuid(manifest.segments[index].uuid));
        // The registry pins one native handle per file, so the directories
        // above stay undeletable until these are retired. Captured here because
        // adopting the merged generation drops the adapter that owns them.
        if (access) superseded_files = access->SegmentFiles();
    }

    const auto staging = directory / ("merge-" + FormatUuid(NewInstanceId()));
    SegmentWriteRequest request;
    request.staging_directory = staging;
    request.dimension = dimension;
    request.block_bytes = block_bytes;
    request.layout = layout;
    request.records = std::move(rows);
    request.builder = builder_name;
    request.builder_version = "1.0.0";
    request.parameters = builder_parameters;
    auto written = WriteSegment(request);
    if (const auto* error = std::get_if<Error>(&written)) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        ++maintenance_errors;
        return *error;
    }
    const auto artifact = std::get<SegmentArtifact>(std::move(written));
    std::error_code path_error;
    const auto segments_directory = directory / "segments";
    std::filesystem::create_directories(segments_directory, path_error);
    if (path_error) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        ++maintenance_errors;
        return Failure(ErrorCode::io_error, "Cannot create the segments directory");
    }
    const auto segment_directory = segments_directory / FormatUuid(artifact.uuid);
    if (!MoveFileExW(staging.c_str(), segment_directory.c_str(), MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        ++maintenance_errors;
        return Error{ErrorCode::io_error, "Cannot move the merged segment into place",
                     static_cast<int>(GetLastError())};
    }

    std::unique_lock<std::shared_mutex> guard(mutex);
    std::lock_guard<std::mutex> publication(publication_mutex);
    if (closed) {
        recycle_queue.push_back(segment_directory);
        return Failure(ErrorCode::closed, "Engine closed while the merge was staged");
    }
    Manifest next = manifest;
    next.generation = manifest.generation + 1;
    next.checkpoint_sequence = (std::max)(manifest.checkpoint_sequence, materialized);
    next.wal_replay_after = next.checkpoint_sequence;
    // A merged segment starts from exactly the rows it holds; the delta keeps
    // whatever arrived afterwards.
    next.live_count = artifact.node_count;
    SegmentId highest = 0;
    for (std::uint32_t index = 0; index < next.segment_count; ++index)
        highest = (std::max)(highest, next.segments[index].id);
    // One merged segment replaces the whole set, so its slot is reused and the
    // superseded entries leave the manifest in the same publish.
    auto& entry = next.segments[0];
    entry = ManifestEntry{};
    entry.id = highest + 1;
    entry.uuid = artifact.uuid;
    entry.generation = next.generation;
    entry.kind = SegmentKind::merged;
    entry.covered_sequence = materialized;
    entry.node_count = artifact.node_count;
    entry.file_bytes = artifact.graph_bytes;
    entry.content_sha256 = artifact.graph_sha256;
    const auto metadata_text = SetManifestText(
        entry.metadata.path, "segments/" + FormatUuid(artifact.uuid) + "/segment.json");
    if (const auto* error = std::get_if<Error>(&metadata_text)) {
        recycle_queue.push_back(segment_directory);
        return *error;
    }
    entry.metadata.logical_bytes = artifact.metadata_bytes;
    entry.metadata.physical_bytes = artifact.metadata_bytes;
    entry.metadata.sha256 = artifact.metadata_sha256;
    next.segment_count = 1;

    const auto published = PublishManifest(directory, next);
    if (const auto* error = std::get_if<Error>(&published)) {
        recycle_queue.push_back(segment_directory);
        ++maintenance_errors;
        return *error;
    }
    auto opened = OpenIndexDirectory(directory, reader, registry, options.block_bytes,
                                     options.max_inflight_reads);
    if (const auto* error = std::get_if<Error>(&opened)) {
        recovery_required = true;
        recovery_message = "Published merged generation " + std::to_string(next.generation) +
                           " but could not reopen it: " + error->message;
        ++maintenance_errors;
        last_maintenance_error = recovery_message;
        return Error{ErrorCode::recovery_required, recovery_message};
    }
    const auto adopted = AdoptPublished(std::get<OpenedIndex>(std::move(opened)));
    if (const auto* error = std::get_if<Error>(&adopted)) return *error;
    if (delta) static_cast<void>(delta->RetirePrefix(retire_count));
    PublishView();
    merge_bytes_written += artifact.staged_bytes;
    ++compactions_completed;
    for (const auto& stale : superseded) recycle_queue.push_back(stale);
    // Order matters: the new generation is live before its predecessor's files
    // are retired, and only a retired file can have its directory removed.
    for (const auto file : superseded_files) static_cast<void>(registry->Retire(file));
    static_cast<void>(registry->CollectRetired());
    const bool fully_covered = published_at_copy == materialized;
    guard.unlock();
    // Reclaim only when the merged segment covers everything that was
    // published. Otherwise the closed WAL file still holds unflushed records
    // and the next checkpoint will delete it once it covers them.
    if (fully_covered) RecycleWalUpTo(materialized);
    RecycleDue(8);
    return Success{};
}

void Engine::Impl::MaintenanceLoop() {
    // Records seen by the previous wakeup. A stream that stopped leaving new
    // records has settled: whatever it left behind is flushed even when it is
    // below the trigger, so a bounded burst cannot sit in the delta forever.
    std::uint32_t previous_committed = 0;
    while (!stopping.load()) {
        {
            std::unique_lock<std::mutex> wait(maintenance_wait_mutex);
            maintenance_cv.wait_for(wait, options.maintenance_interval,
                                    [this] { return stopping.load(); });
        }
        if (stopping.load()) break;
        std::uint32_t committed = 0;
        std::uint32_t capacity = 0;
        std::uint32_t segments = 0;
        {
            std::shared_lock<std::shared_mutex> guard(mutex);
            if (closed || recovery_required || !delta) continue;
            committed = delta->CommittedCount();
            capacity = delta->SlotCapacity();
            segments = manifest.segment_count;
        }
        const std::uint32_t trigger = options.checkpoint_trigger_vectors != 0
                                          ? options.checkpoint_trigger_vectors
                                          : (std::max)(1u, capacity / 2);
        const bool settled =
            options.maintenance_settle_flush && committed > 0 && committed == previous_committed;
        previous_committed = committed;
        // A merge only ever reads segments, so a flush that is due always runs
        // first. Both are attempted in the same wakeup: under a sustained write
        // stream the flush branch would otherwise starve the merge forever.
        if (committed >= trigger || settled) {
            const auto done = RunCheckpoint(std::chrono::seconds(60));
            if (const auto* error = std::get_if<Error>(&done)) {
                last_maintenance_error = error->message;
                ++maintenance_errors;
            }
            previous_committed = 0;
            std::shared_lock<std::shared_mutex> guard(mutex);
            segments = closed ? segments : manifest.segment_count;
        }
        if (segments > options.max_delta_segments) {
            const auto done = RunCompaction(std::chrono::seconds(120));
            if (const auto* error = std::get_if<Error>(&done)) {
                last_maintenance_error = error->message;
                ++maintenance_errors;
            }
            // A merge ends with its own flush, so the next wakeup starts clean.
            previous_committed = 0;
        }
        RecycleDue(4);
    }
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

    // --- stage 6: open the write path and replay what the WAL still owes ----
    impl->published_sequence = impl->manifest.checkpoint_sequence;
    const auto frame_bytes = WalFrameBytes(impl->manifest.dimension);
    if (const auto* error = std::get_if<Error>(&frame_bytes)) return *error;
    impl->frame_bytes = std::get<std::uint64_t>(frame_bytes);
    if (impl->frame_bytes > options.wal.max_frame_bytes)
        return Failure(ErrorCode::invalid_argument,
                       "The configured WAL frame limit is smaller than one record");

    impl->wal_directory = directory / "wal";
    {
        std::error_code wal_error;
        std::filesystem::create_directories(impl->wal_directory, wal_error);
        if (wal_error)
            return Failure(ErrorCode::io_error,
                           "Cannot create the WAL directory: " + impl->wal_directory.string());
    }

    auto recovery = ScanWalDirectory(impl->wal_directory, impl->manifest.instance_id,
                                     impl->manifest.dimension, impl->published_sequence,
                                     options.wal);
    if (const auto* error = std::get_if<Error>(&recovery)) return *error;
    auto report = std::get<RecoveryReport>(std::move(recovery));

    // The delta is bounded twice: by the memory budget it has to live inside,
    // and by the retention limit the resource contract declares.
    const auto record_bytes = DeltaRecordBytes(impl->manifest.dimension);
    std::uint64_t capacity = options.memory.delta_bytes / record_bytes;
    capacity = (std::min)(capacity,
                          static_cast<std::uint64_t>(options.resources.max_retained_delta_vectors));
    capacity = (std::min)(capacity,
                          static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max() / 2));
    if (report.records.size() > capacity)
        return Failure(ErrorCode::resource_exhausted,
                       "The WAL holds more records than the delta can retain");
    if (capacity > 0) {
        DeltaOptions delta_options;
        delta_options.dimension = impl->manifest.dimension;
        delta_options.slot_capacity = static_cast<std::uint32_t>(capacity);
        auto layer = DeltaLayer::Create(delta_options, impl->governor);
        if (const auto* error = std::get_if<Error>(&layer)) return *error;
        impl->delta = std::get<std::unique_ptr<DeltaLayer>>(std::move(layer));
        impl->delta_resident_bytes = impl->delta->ResidentBytes();
    }

    impl->replayed_records = report.records.size();
    impl->discarded_tail_bytes = report.discarded_tail_bytes;
    impl->wal_files_scanned = report.files.size();
    if (!report.records.empty()) {
        // The count is over IDs, not records: a second version and an overwrite
        // of a segmented vector are not new vectors. The write path applies the
        // same rule, so replay must not diverge from it just because the history
        // happens to hold several rows for one ID.
        std::unordered_set<VectorId> counted;
        std::uint64_t new_ids = 0;
        for (const auto& record : report.records) {
            const auto restored = impl->delta->RestoreRecord(record.id, record.sequence,
                                                             record.values);
            if (const auto* error = std::get_if<Error>(&restored)) return *error;
            impl->logical_update_bytes += record.values.size() * sizeof(float);
            if (!counted.insert(record.id).second) continue;
            auto in_base = impl->ExistsInBase(record.id);
            if (const auto* error = std::get_if<Error>(&in_base)) return *error;
            if (!std::get<bool>(in_base)) ++new_ids;
        }
        impl->live_count = impl->manifest.live_count + new_ids;
        // Replayed IDs are visible but not stored in any segment yet, exactly
        // like a fresh write; the next publish has to account for them.
        impl->pending_new_ids = static_cast<std::uint32_t>(new_ids);
    }
    if (report.highest_sequence > impl->published_sequence)
        impl->published_sequence = report.highest_sequence;

    if (report.latest) {
        const auto& latest = *report.latest;
        auto finished = wal_detail::FinishWalRecovery(latest.file, latest.valid_bytes,
            latest.header, impl->manifest.dimension, options.wal);
        if (const auto* error = std::get_if<Error>(&finished)) return *error;
        impl->wal = std::get<std::unique_ptr<WalWriter>>(std::move(finished));
        if (impl->wal) impl->wal_file_bytes = latest.valid_bytes;
    }

    impl->view.instance_id = impl->manifest.instance_id;
    impl->view.view_generation = 1;
    impl->view.manifest_generation = impl->manifest.generation;
    impl->PublishView();
    auto engine = std::unique_ptr<Engine>(new Engine(std::move(impl)));
    // Maintenance is opt-in: without rebuild_memory the caller has not declared
    // the workspace a checkpoint or merge would need, so nothing runs behind
    // their back. Checkpoint() still works when called explicitly.
    if (engine->impl_->MaintenanceEnabled()) {
        Engine::Impl* worker = engine->impl_.get();
        worker->maintenance = std::thread([worker] { worker->MaintenanceLoop(); });
    }
    return engine;
}

Result<WriteReceipt> Engine::Impl::ApplyWrite(WalOperation operation, VectorId id,
                                              std::span<const float> values,
                                              const WriteOptions& write_options) {
    if (id == 0) return Failure(ErrorCode::invalid_argument, "Logical ID must be non-zero");
    if (values.size() != manifest.dimension)
        return Failure(ErrorCode::invalid_argument, "Write vector dimension mismatch");
    if (!AllFinite(values))
        return Failure(ErrorCode::invalid_argument,
                       "Write vector must contain finite float32 values");
    if (write_options.admission_timeout.count() <= 0)
        return Failure(ErrorCode::invalid_argument, "Write admission timeout must be positive");
    if (options.max_pending_writes == 0)
        return Failure(ErrorCode::invalid_argument, "max_pending_writes must be positive");

    // Admission happens before the publication lock: the counter bounds how
    // many writers can queue, so a stalled disk cannot build an unbounded
    // backlog of threads holding reserved slots.
    const auto deadline = std::chrono::steady_clock::now() + write_options.admission_timeout;
    while (true) {
        auto current = pending_writes.load();
        if (current < options.max_pending_writes) {
            if (pending_writes.compare_exchange_weak(current, current + 1)) break;
            continue;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return Failure(ErrorCode::resource_exhausted, "Write admission capacity exhausted");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    struct PendingGuard {
        std::atomic<std::uint32_t>* counter;
        ~PendingGuard() { counter->fetch_sub(1); }
    } pending_guard{&pending_writes};

    std::unique_lock<std::shared_mutex> guard(mutex);
    if (closed) return Failure(ErrorCode::closed, "Engine is closed");
    if (recovery_required) return Error{ErrorCode::recovery_required, recovery_message};
    if (!delta)
        return Failure(ErrorCode::resource_exhausted,
                       "No delta budget is configured; writes are disabled");

    std::lock_guard<std::mutex> publication(publication_mutex);

    const auto existing = VisibleVersion(id, published_sequence);
    if (const auto* error = std::get_if<Error>(&existing)) return *error;
    const bool existed = std::get<std::optional<Sequence>>(existing).has_value();
    if (operation == WalOperation::insert && existed)
        return Failure(ErrorCode::already_exists, "Logical ID already exists in the index");

    if (published_sequence == (std::numeric_limits<Sequence>::max)())
        return Failure(ErrorCode::resource_exhausted, "WAL sequence space is exhausted");
    const Sequence sequence = published_sequence + 1;
    const auto pin = delta->CapturePin();
    auto reserved = delta->ReserveSlot();
    if (const auto* error = std::get_if<Error>(&reserved)) return *error;
    const SlotId slot = std::get<SlotId>(reserved);
    auto release_slot = [this, slot]() { static_cast<void>(delta->CancelLastSlot(slot)); };

    const auto stored = delta->WriteSlot(slot, id, sequence, values);
    if (const auto* error = std::get_if<Error>(&stored)) {
        release_slot();
        return *error;
    }

    // Capacity is settled before the first byte: a group is never split because
    // it turned out not to fit.
    const auto stop_after_wal_io = [this](const Error& error) {
        // An empty log may already have been published before attachment failed.
        if (error.code == ErrorCode::io_error) {
            recovery_required = true;
            recovery_message = error.message;
        }
    };
    if (!wal) {
        WalFileHeader header{};
        header.instance_id = manifest.instance_id;
        header.start_sequence = sequence;
        auto created = CreateWalWriter(wal_directory, header, manifest.dimension, options.wal);
        if (const auto* error = std::get_if<Error>(&created)) {
            stop_after_wal_io(*error);
            release_slot();
            return *error;
        }
        wal = std::get<std::unique_ptr<WalWriter>>(std::move(created));
        wal_file_bytes = wal_detail::kWalHeaderBytes;
    } else if (wal_file_bytes + frame_bytes > options.wal.rotation_bytes) {
        WalFileHeader next{};
        next.instance_id = manifest.instance_id;
        next.start_sequence = sequence;
        auto rotated = wal->Rotate(next);
        if (const auto* error = std::get_if<Error>(&rotated)) {
            stop_after_wal_io(*error);
            release_slot();
            return *error;
        }
        wal_file_bytes = wal_detail::kWalHeaderBytes;
    }

    std::vector<std::byte> frame(static_cast<std::size_t>(frame_bytes));
    const WalRecord record{operation, sequence, id, values};
    auto encoded = EncodeWalFrame(record, frame);
    if (const auto* error = std::get_if<Error>(&encoded)) {
        release_slot();
        return *error;
    }

    auto appended = wal->Append(frame);
    if (auto* error = std::get_if<Error>(&appended)) {
        // The writer promised not to write again after a failed append. The slot
        // is unpublished, so taking it back keeps this process consistent with
        // the torn tail a restart would discard.
        error->attempted_sequence = sequence;
        recovery_required = true;
        recovery_message = error->message;
        release_slot();
        return *error;
    }

    auto synced = wal->Sync();
    if (auto* error = std::get_if<Error>(&synced)) {
        // Sync failed: the record may already be on disk. The slot stays
        // reserved but unpublished: invisible here, possibly visible after a
        // restart. That is exactly may_have_applied.
        recovery_required = true;
        recovery_message = error->message;
        error->write_effect = WriteEffect::may_have_applied;
        error->attempted_sequence = sequence;
        return *error;
    }

    wal_file_bytes += frame_bytes;
    wal_bytes_written += frame_bytes;
    logical_update_bytes += static_cast<std::uint64_t>(values.size()) * sizeof(float);

    const auto moved = delta->PublishPrefix(pin.committed_prefix + 1);
    if (const auto* error = std::get_if<Error>(&moved)) {
        recovery_required = true;
        recovery_message = error->message;
        return *error;
    }
    published_sequence = sequence;
    if (!existed) {
        ++live_count;
        ++pending_new_ids;
    }
    PublishView();
    return WriteReceipt{id, sequence};
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

    // The graph cannot contain an ID written after the last checkpoint, so the
    // delta is re-ranked on its own and the two sets are merged by version.
    // A newer delta record replaces the graph's older one; the graph's copy is
    // never allowed to win on distance alone.
    std::unordered_map<VectorId, std::pair<Sequence, double>> ranked;
    const auto consider = [&ranked](VectorId candidate_id, Sequence version, double distance) {
        const auto existing = ranked.find(candidate_id);
        if (existing == ranked.end() || existing->second.first < version)
            ranked[candidate_id] = {version, distance};
    };
    for (const auto& candidate : candidates) {
        if (!candidate.exact_distance.has_value()) continue;
        consider(candidate.id, candidate.version, *candidate.exact_distance);
    }
    std::uint64_t delta_scanned = 0;
    if (impl.delta) {
        for (std::uint32_t delta_index = 0; delta_index < lease.view.delta_count; ++delta_index) {
            const auto& pin = lease.view.deltas[delta_index];
            // A pin from before a checkpoint names a retired segment; its
            // records are in a segment file by now, not in this layer.
            if (pin.segment != impl.delta->CurrentSegmentId()) continue;
            for (std::uint32_t index = 0; index < pin.committed_prefix; ++index) {
                const auto version = impl.delta->VersionAt(index);
                if (version > lease.view.snapshot_sequence) continue;
                ++delta_scanned;
                const auto values = impl.delta->ValuesAt(index);
                consider(impl.delta->IdAt(index), version,
                         SquaredL2(query.data(), values.data(), impl.manifest.dimension));
            }
        }
    }

    std::vector<Neighbor> ordered;
    ordered.reserve(ranked.size());
    for (const auto& item : ranked) {
        Neighbor neighbour;
        neighbour.id = item.first;
        neighbour.version = item.second.first;
        neighbour.squared_distance = item.second.second;
        ordered.push_back(neighbour);
    }
    std::sort(ordered.begin(), ordered.end(), [](const Neighbor& left, const Neighbor& right) {
        if (left.squared_distance != right.squared_distance)
            return left.squared_distance < right.squared_distance;
        return left.id < right.id;
    });
    if (ordered.size() > options.k) ordered.resize(options.k);
    result.neighbors = std::move(ordered);
    result.metrics.cache_hits = query_reads->cache_hits;
    result.metrics.cache_misses = query_reads->cache_misses;
    result.metrics.logical_block_demands = result.metrics.cache_hits + result.metrics.cache_misses;
    result.metrics.delta_records_scanned = delta_scanned;
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

    // The delta answers first when it holds a newer version, because the graph
    // predates it. Both sides are filtered by the captured snapshot, so a Get
    // that started before a write cannot return the newer record.
    std::uint32_t delta_slot = 0;
    Sequence delta_version = 0;
    bool has_delta = false;
    if (impl.delta) {
        for (std::uint32_t delta_index = 0; delta_index < view.delta_count; ++delta_index) {
            const auto& pin = view.deltas[delta_index];
            if (pin.segment != impl.delta->CurrentSegmentId()) continue;
            for (std::uint32_t index = 0; index < pin.committed_prefix; ++index) {
                if (impl.delta->IdAt(index) != id) continue;
                const auto version = impl.delta->VersionAt(index);
                if (version > view.snapshot_sequence) continue;
                if (!has_delta || version > delta_version) {
                    has_delta = true;
                    delta_version = version;
                    delta_slot = index;
                }
            }
        }
    }

    std::optional<VersionDirectoryEntry> best;
    // The node id in a directory entry only means something inside the segment
    // that produced it, so the winning segment travels with the entry.
    SegmentId best_segment = 0;
    for (std::uint32_t index = 0; index < view.segment_count; ++index) {
        const auto found = impl.access->FindId(view.segments[index].id, id);
        if (const auto* error = std::get_if<Error>(&found)) return *error;
        const auto& entry = std::get<std::optional<VersionDirectoryEntry>>(found);
        if (!entry.has_value()) continue;
        if (entry->version > view.snapshot_sequence) continue;
        if (!best.has_value() || best->version < entry->version) {
            best = entry;
            best_segment = view.segments[index].id;
        }
    }

    if (has_delta && (!best.has_value() || best->version < delta_version)) {
        StoredVector stored;
        stored.id = id;
        stored.version = delta_version;
        stored.snapshot_sequence = view.snapshot_sequence;
        const auto values = impl.delta->ValuesAt(delta_slot);
        stored.values.assign(values.begin(), values.end());
        return stored;
    }
    if (!best.has_value()) return Failure(ErrorCode::not_found, "Logical ID is not visible");
    const auto stored = impl.access->ReadVector(best_segment, best->node, view.snapshot_sequence);
    if (const auto* error = std::get_if<Error>(&stored)) {
        if (error->code == ErrorCode::not_found)
            return Failure(ErrorCode::not_found, "Logical ID is not visible at this snapshot");
        return *error;
    }
    const auto& vector = std::get<StoredVector>(stored);
    if (vector.id != id)
        return Failure(ErrorCode::corruption,
                       "The version directory points at a row that holds another ID");
    return vector;
}

Result<WriteReceipt> Engine::Insert(VectorId id, std::span<const float> values,
                                   const WriteOptions& options) {
    if (!impl_) return Failure(ErrorCode::closed, "Engine is closed");
    return impl_->ApplyWrite(WalOperation::insert, id, values, options);
}

Result<WriteReceipt> Engine::Upsert(VectorId id, std::span<const float> values,
                                   const WriteOptions& options) {
    if (!impl_) return Failure(ErrorCode::closed, "Engine is closed");
    return impl_->ApplyWrite(WalOperation::upsert, id, values, options);
}

Result<CheckpointReceipt> Engine::Checkpoint(std::chrono::milliseconds timeout) {
    if (!impl_) return Failure(ErrorCode::closed, "Engine is closed");
    if (timeout.count() <= 0)
        return Failure(ErrorCode::invalid_argument, "Checkpoint timeout must be positive");
    return impl_->RunCheckpoint(timeout);
}

Result<EngineStats> Engine::Stats() const {
    if (!impl_) return Failure(ErrorCode::closed, "Engine is closed");
    const Impl& impl = *impl_;
    std::shared_lock guard(impl.mutex);
    EngineStats stats;
    stats.published_sequence = impl.published_sequence;
    stats.checkpoint_sequence = impl.manifest.checkpoint_sequence;
    stats.view_generation = impl.view.view_generation;
    stats.live_vectors = impl.live_count;
    if (impl.governor) stats.memory_accounted_bytes = MemoryAccountedBytes(*impl.governor);
    stats.delta_resident_bytes = impl.delta_resident_bytes;
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
    stats.wal_bytes_written = impl.wal_bytes_written;
    stats.checkpoint_bytes_written = impl.checkpoint_bytes_written;
    stats.merge_bytes_written = impl.merge_bytes_written;
    stats.logical_update_bytes = impl.logical_update_bytes;
    stats.pending_write_count = impl.pending_writes.load();
    // Unflushed delta is the merge backlog: bytes that are durable in the WAL
    // but still have to be written again as a segment.
    stats.merge_backlog_bytes = impl.delta && impl.manifest.dimension != 0
        ? static_cast<std::uint64_t>(impl.delta->CommittedCount()) *
              DeltaRecordBytes(impl.manifest.dimension)
        : 0;
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
    Impl& impl = *impl_;
    // The maintenance worker may be waiting for the view lock, so it is told to
    // stop before the lock is taken, and joined before the engine is marked
    // closed: a merge that is already staging still gets to publish or discard
    // itself instead of racing the teardown. Waiting inside the critical
    // section would deadlock.
    impl.RequestStop();
    impl.JoinMaintenance();
    {
        std::unique_lock guard(impl.mutex);
        if (impl.closed) return Failure(ErrorCode::closed, "Engine is already closed");
        impl.closed = true;
    }
    impl.Shutdown();
    return Success{};
}

}  // namespace agent_io
