#include "agent_io/synchronous_storage.hpp"

#include <windows.h>
#include <malloc.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <utility>

namespace agent_io {
namespace {

Error Invalid(const char* message) {
    return {ErrorCode::invalid_argument, message};
}

Error Exhausted() {
    return {ErrorCode::resource_exhausted, "Storage capacity exhausted"};
}

Error SystemError(const char* operation, DWORD code) {
    const auto kind = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND
        ? ErrorCode::not_found : ErrorCode::io_error;
    return {kind, operation, static_cast<int>(code)};
}

bool ValidBudget(const MemoryBudget& budget) {
    if (!budget.total_bytes || !budget.metadata_bytes ||
        !budget.workspace_bytes || !budget.reserve_bytes) return false;
    std::uint64_t remaining = budget.total_bytes;
    for (const auto bytes : {budget.cache_bytes, budget.delta_bytes,
             budget.metadata_bytes, budget.workspace_bytes, budget.reserve_bytes}) {
        if (bytes > remaining) return false;
        remaining -= bytes;
    }
    return remaining == 0;
}

std::array<std::uint64_t, 4> Limits(const MemoryBudget& budget) {
    return {budget.cache_bytes, budget.delta_bytes,
            budget.metadata_bytes, budget.workspace_bytes};
}

struct BudgetState {
    std::mutex mutex;
    std::array<std::uint64_t, 4> used{};
    MemoryBudget query;
    std::optional<MemoryBudget> rebuild;
    BudgetMode mode = BudgetMode::query;
};

// Covers token/control-block bookkeeping conservatively, not OS working set.
constexpr std::uint64_t reservation_overhead = 256;

class Reservation final : public BudgetReservation {
    std::shared_ptr<BudgetState> state_;
    MemoryCharge charge_;
public:
    Reservation(std::shared_ptr<BudgetState> state, MemoryCharge charge)
        : state_(std::move(state)), charge_(charge) {}
    ~Reservation() override {
        std::lock_guard lock(state_->mutex);
        state_->used[static_cast<std::size_t>(charge_.region)] -= charge_.bytes;
        state_->used[2] -= reservation_overhead;
    }
    MemoryCharge Charge() const noexcept override { return charge_; }
};

class Governor final : public ResourceGovernor {
    std::shared_ptr<BudgetState> state_;
public:
    Governor(const MemoryBudget& query, std::optional<MemoryBudget> rebuild)
        : state_(std::make_shared<BudgetState>()) {
        state_->query = query;
        state_->rebuild = rebuild;
        state_->used[2] = sizeof(BudgetState) + sizeof(Governor) + 256;
    }
    Result<std::shared_ptr<const BudgetReservation>> TryReserve(
        MemoryCharge charge) override {
        const auto region = static_cast<std::size_t>(charge.region);
        if (region >= 4 || charge.bytes == 0) return Invalid("Invalid memory charge");
        std::lock_guard lock(state_->mutex);
        const auto limits = Limits(state_->mode == BudgetMode::query
            ? state_->query : *state_->rebuild);
        auto wanted = state_->used;
        if (reservation_overhead > limits[2] - wanted[2]) return Exhausted();
        wanted[2] += reservation_overhead;
        if (charge.bytes > limits[region] - wanted[region]) return Exhausted();
        wanted[region] += charge.bytes;
        try {
            auto token = std::make_shared<Reservation>(state_, charge);
            state_->used = wanted;
            return std::shared_ptr<const BudgetReservation>(std::move(token));
        } catch (const std::bad_alloc&) {
            return Exhausted();
        }
    }
    Result<Success> TrySwitchMode(BudgetMode mode) override {
        if (mode != BudgetMode::query && mode != BudgetMode::rebuild)
            return Invalid("Invalid budget mode");
        std::lock_guard lock(state_->mutex);
        if (mode == BudgetMode::rebuild && !state_->rebuild)
            return Error{ErrorCode::unsupported, "Rebuild budget not configured"};
        const auto limits = Limits(mode == BudgetMode::query
            ? state_->query : *state_->rebuild);
        for (std::size_t i = 0; i < limits.size(); ++i)
            if (state_->used[i] > limits[i]) return Exhausted();
        state_->mode = mode;
        return Success{};
    }
    BudgetMode Mode() const noexcept override {
        std::lock_guard lock(state_->mutex);
        return state_->mode;
    }
};

class Handle {
    HANDLE value_;
public:
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const { return value_; }
    HANDLE Release() { return std::exchange(value_, INVALID_HANDLE_VALUE); }
};

struct FileIdentity {
    ULONGLONG volume;
    std::array<unsigned char, 16> id;
    auto operator<=>(const FileIdentity&) const = default;
};

class NativeFile final : public FileLease {
    std::shared_ptr<const BudgetReservation> reservation_;
    Handle handle_;
    FileId id_;
    std::uint64_t size_;
public:
    const std::uint32_t alignment;
    mutable std::mutex io_mutex;
    NativeFile(std::shared_ptr<const BudgetReservation> reservation, HANDLE handle,
               FileId id, std::uint64_t size, std::uint32_t align)
        : reservation_(std::move(reservation)), handle_(handle),
          id_(id), size_(size), alignment(align) {}
    HANDLE NativeHandle() const { return handle_.Get(); }
    FileId Id() const noexcept override { return id_; }
    std::uint64_t SizeBytes() const noexcept override { return size_; }
};

std::atomic<FileId> next_file_id{1};

Result<FileId> NextFileId() {
    auto current = next_file_id.load();
    while (current != (std::numeric_limits<FileId>::max)()) {
        if (next_file_id.compare_exchange_weak(current, current + 1)) return current;
    }
    return Exhausted();
}

struct FileEntry {
    std::shared_ptr<NativeFile> file;
    bool retired = false;
};

class Registry final : public FileRegistry {
    std::shared_ptr<ResourceGovernor> governor_;
    SynchronousStorageOptions options_;
    std::shared_ptr<const BudgetReservation> reservation_;
    std::mutex mutex_;
    std::map<FileIdentity, FileEntry> entries_;
public:
    Registry(std::shared_ptr<ResourceGovernor> governor,
             SynchronousStorageOptions options,
             std::shared_ptr<const BudgetReservation> reservation)
        : governor_(std::move(governor)), options_(options),
          reservation_(std::move(reservation)) {}
    Result<std::shared_ptr<const FileLease>> Open(
        const std::filesystem::path& path) override {
        try {
            // No write/delete sharing: immutable data cannot change under a lease.
            Handle handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, options_.direct_reads
                    ? FILE_FLAG_NO_BUFFERING : FILE_ATTRIBUTE_NORMAL, nullptr));
            if (handle.Get() == INVALID_HANDLE_VALUE)
                return SystemError("CreateFileW read", GetLastError());
            FILE_ID_INFO identity{};
            LARGE_INTEGER size{};
            if (!GetFileInformationByHandleEx(handle.Get(), FileIdInfo,
                    &identity, sizeof(identity)))
                return SystemError("FileIdInfo", GetLastError());
            if (!GetFileSizeEx(handle.Get(), &size))
                return SystemError("GetFileSizeEx", GetLastError());
            FileIdentity key{identity.VolumeSerialNumber, {}};
            std::memcpy(key.id.data(), identity.FileId.Identifier, key.id.size());
            std::lock_guard lock(mutex_);
            if (const auto found = entries_.find(key); found != entries_.end()) {
                if (found->second.retired)
                    return Error{ErrorCode::closed, "Retired file cannot be reopened"};
                return std::shared_ptr<const FileLease>(found->second.file);
            }
            if (entries_.size() >= options_.max_registered_files) return Exhausted();
            DWORD alignment = 1;
            if (options_.direct_reads) {
                FILE_STORAGE_INFO storage{};
                if (!GetFileInformationByHandleEx(handle.Get(), FileStorageInfo,
                        &storage, sizeof(storage)))
                    return Error{ErrorCode::unsupported, "FileStorageInfo unavailable",
                                 static_cast<int>(GetLastError())};
                alignment = (std::max)({storage.LogicalBytesPerSector,
                    storage.PhysicalBytesPerSectorForAtomicity,
                    storage.PhysicalBytesPerSectorForPerformance});
                if (!storage.LogicalBytesPerSector || !alignment ||
                    (alignment & (alignment - 1)) || alignment > 1024 * 1024)
                    return Error{ErrorCode::unsupported, "Unsupported sector alignment"};
            }
            auto reserved = governor_->TryReserve(
                {MemoryRegion::metadata, sizeof(NativeFile) + sizeof(FileEntry) + 512});
            if (const auto* error = std::get_if<Error>(&reserved)) return *error;
            auto id = NextFileId();
            if (const auto* error = std::get_if<Error>(&id)) return *error;
            auto file = std::make_shared<NativeFile>(
                std::get<std::shared_ptr<const BudgetReservation>>(std::move(reserved)),
                handle.Get(), std::get<FileId>(id),
                static_cast<std::uint64_t>(size.QuadPart), alignment);
            handle.Release();
            entries_.emplace(key, FileEntry{file});
            return std::shared_ptr<const FileLease>(std::move(file));
        } catch (const std::bad_alloc&) {
            return Exhausted();
        }
    }
    Result<Success> Retire(FileId id) override {
        std::lock_guard lock(mutex_);
        for (auto& [key, entry] : entries_) {
            if (entry.file->Id() == id) {
                entry.retired = true;
                return Success{};
            }
        }
        return Error{ErrorCode::not_found, "Unknown file ID"};
    }
    Result<Success> CollectRetired() override {
        std::lock_guard lock(mutex_);
        for (const auto& [key, entry] : entries_) {
            if (entry.retired && entry.file.use_count() == 1)
                return Error{ErrorCode::unsupported,
                    "Automatic deletion disabled pending recovery acceptance"};
        }
        return Success{};
    }
};

class OwnedBuffer final : public BufferLease {
    std::shared_ptr<const BudgetReservation> reservation_;
    std::byte* bytes_;
    std::uint32_t size_;
public:
    OwnedBuffer(std::shared_ptr<const BudgetReservation> reservation,
                std::byte* bytes, std::uint32_t size)
        : reservation_(std::move(reservation)), bytes_(bytes), size_(size) {}
    ~OwnedBuffer() override { _aligned_free(bytes_); }
    std::span<std::byte> WritableBytes() noexcept override { return {bytes_, size_}; }
    std::span<const std::byte> Bytes() const noexcept override { return {bytes_, size_}; }
};

class Backend final : public SynchronousReadBackend {
    std::shared_ptr<const BudgetReservation> reservation_;
    const std::uint32_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable drained_;
    ReadBackendStats stats_;
    struct Active {
        RequestId id = 0;
        std::shared_ptr<BufferLease> buffer;
    };
    std::vector<Active> active_;
    bool closed_ = false;
public:
    Backend(std::uint32_t capacity, std::shared_ptr<const BudgetReservation> reservation)
        : reservation_(std::move(reservation)), capacity_(capacity), active_(capacity) {}
    Result<Success> Submit(ReadRequest request, ReadCallback callback) override {
        auto file = std::dynamic_pointer_cast<const NativeFile>(request.file);
        const auto& block = request.block;
        if (!callback || !file || !request.buffer ||
            block.file_id != file->Id() || block.byte_count == 0 ||
            block.aligned_offset > file->SizeBytes() ||
            block.byte_count > file->SizeBytes() - block.aligned_offset ||
            block.aligned_offset > static_cast<std::uint64_t>(
                (std::numeric_limits<LONGLONG>::max)()) ||
            request.buffer->WritableBytes().size() < block.byte_count ||
            !request.buffer->WritableBytes().data() ||
            block.aligned_offset % file->alignment ||
            block.byte_count % file->alignment ||
            reinterpret_cast<std::uintptr_t>(
                request.buffer->WritableBytes().data()) % file->alignment ||
            (request.priority != ReadPriority::demand &&
             request.priority != ReadPriority::background))
            return Invalid("Invalid synchronous read request");
        std::size_t slot = 0;
        {
            std::lock_guard lock(mutex_);
            if (closed_) return Error{ErrorCode::closed, "Backend drained"};
            const auto address = reinterpret_cast<std::uintptr_t>(
                request.buffer->WritableBytes().data());
            for (const auto& active : active_) {
                if (!active.buffer) continue;
                const auto other = reinterpret_cast<std::uintptr_t>(
                    active.buffer->WritableBytes().data());
                const auto count = active.buffer->WritableBytes().size();
                if (active.id == request.request_id ||
                    (address >= other ? address - other < count
                        : other - address < request.buffer->WritableBytes().size()))
                    return Invalid("Duplicate in-flight ID or overlapping buffer");
            }
            if (stats_.inflight_reads >= capacity_) return Exhausted();
            while (active_[slot].buffer) ++slot;
            active_[slot] = {request.request_id, request.buffer};
            ++stats_.inflight_reads;
        }
        DWORD completed = 0;
        DWORD failure = ERROR_SUCCESS;
        bool submitted = false;
        {
            // Synchronous handles share a file pointer, including across backends.
            std::lock_guard file_lock(file->io_mutex);
            LARGE_INTEGER offset{};
            offset.QuadPart = static_cast<LONGLONG>(block.aligned_offset);
            if (!SetFilePointerEx(file->NativeHandle(), offset, nullptr, FILE_BEGIN)) {
                failure = GetLastError();
            } else {
                submitted = true;
                if (!ReadFile(file->NativeHandle(),
                        request.buffer->WritableBytes().data(),
                        block.byte_count, &completed, nullptr))
                    failure = GetLastError();
                else if (completed != block.byte_count)
                    failure = ERROR_HANDLE_EOF;
            }
        }
        {
            std::lock_guard lock(mutex_);
            stats_.io_reads_submitted += submitted ? 1 : 0;
            stats_.io_read_bytes_completed += completed;
            stats_.io_read_errors += failure != ERROR_SUCCESS ? 1 : 0;
        }
        // The callback contract requires noexcept behavior and forbids Drain here.
        // Keep the admission slot and leases until the callback has returned.
        try {
            if (failure != ERROR_SUCCESS)
                callback(SystemError("Synchronous read failed or short", failure));
            else
                callback(ReadCompletion{request.request_id, completed});
        } catch (...) {
            request.file.reset();
            request.buffer.reset();
            file.reset();
            Finish(slot);
            throw;
        }
        request.file.reset();
        request.buffer.reset();
        file.reset();
        Finish(slot);
        return Success{};
    }
    void Finish(std::size_t slot) {
        std::lock_guard lock(mutex_);
        active_[slot].buffer.reset();
        --stats_.inflight_reads;
        drained_.notify_all();
    }
    Result<Success> Drain() override {
        std::unique_lock lock(mutex_);
        closed_ = true;
        drained_.wait(lock, [&] { return stats_.inflight_reads == 0; });
        return Success{};
    }
    ReadBackendStats Stats() const override {
        std::lock_guard lock(mutex_);
        return stats_;
    }
};

}  // namespace

Result<std::shared_ptr<ResourceGovernor>> CreateResourceGovernor(
    const MemoryBudget& query, const std::optional<MemoryBudget>& rebuild) {
    if (!ValidBudget(query) || (rebuild &&
        (!ValidBudget(*rebuild) || rebuild->total_bytes != query.total_bytes)))
        return Invalid("Invalid query/rebuild budget");
    const auto bootstrap_bytes = sizeof(BudgetState) + sizeof(Governor) + 256;
    if (query.metadata_bytes < bootstrap_bytes ||
        (rebuild && rebuild->metadata_bytes < bootstrap_bytes)) return Exhausted();
    try {
        return std::shared_ptr<ResourceGovernor>(std::make_shared<Governor>(query, rebuild));
    } catch (const std::bad_alloc&) {
        return Exhausted();
    }
}

Result<std::unique_ptr<FileRegistry>> CreateSynchronousFileRegistry(
    std::shared_ptr<ResourceGovernor> governor, const SynchronousStorageOptions& options) {
    if (!governor || !options.max_registered_files) return Invalid("Invalid registry options");
    auto reserved = governor->TryReserve({MemoryRegion::metadata, sizeof(Registry) + 256});
    if (const auto* error = std::get_if<Error>(&reserved)) return *error;
    try {
        return std::unique_ptr<FileRegistry>(std::make_unique<Registry>(
            std::move(governor), options,
            std::get<std::shared_ptr<const BudgetReservation>>(std::move(reserved))));
    } catch (const std::bad_alloc&) {
        return Exhausted();
    }
}

Result<std::shared_ptr<BufferLease>> AllocateReadBuffer(
    std::shared_ptr<ResourceGovernor> governor, std::uint32_t bytes,
    std::uint32_t alignment, MemoryRegion region) {
    if (!governor || !bytes || !alignment || (alignment & (alignment - 1)) ||
        alignment > 1024 * 1024 ||
        (region != MemoryRegion::workspace && region != MemoryRegion::cache))
        return Invalid("Invalid read buffer options");
    alignment = (std::max)(alignment, static_cast<std::uint32_t>(sizeof(void*)));
    auto reserved = governor->TryReserve(
        {region, static_cast<std::uint64_t>(bytes) + alignment + sizeof(OwnedBuffer) + 256});
    if (const auto* error = std::get_if<Error>(&reserved)) return *error;
    auto* data = static_cast<std::byte*>(_aligned_malloc(bytes, alignment));
    if (!data) return Exhausted();
    try {
        return std::shared_ptr<BufferLease>(std::make_shared<OwnedBuffer>(
            std::get<std::shared_ptr<const BudgetReservation>>(std::move(reserved)),
            data, bytes));
    } catch (const std::bad_alloc&) {
        _aligned_free(data);
        return Exhausted();
    }
}

Result<std::uint32_t> ReadAlignment(const FileLease& file) {
    if (const auto* native = dynamic_cast<const NativeFile*>(&file)) return native->alignment;
    return Invalid("File lease was not created by synchronous registry");
}

Result<std::unique_ptr<SynchronousReadBackend>> CreateSynchronousReadBackend(
    std::shared_ptr<ResourceGovernor> governor, const SynchronousStorageOptions& options) {
    if (!governor || !options.max_inflight_reads) return Invalid("Invalid read capacity");
    auto reserved = governor->TryReserve({MemoryRegion::metadata,
        sizeof(Backend) + static_cast<std::uint64_t>(options.max_inflight_reads) *
            (sizeof(RequestId) + sizeof(std::shared_ptr<BufferLease>)) + 256});
    if (const auto* error = std::get_if<Error>(&reserved)) return *error;
    try {
        return std::unique_ptr<SynchronousReadBackend>(
            std::make_unique<Backend>(options.max_inflight_reads,
                std::get<std::shared_ptr<const BudgetReservation>>(std::move(reserved))));
    } catch (const std::bad_alloc&) {
        return Exhausted();
    }
}

}  // namespace agent_io
