#include "agent_io/optimized_storage.hpp"

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
#include <thread>
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
    std::uint64_t AccountedBytes() const {
        std::lock_guard lock(state_->mutex);
        const auto& used = state_->used;
        return used[0] + used[1] + used[2] + used[3];
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
    const bool overlapped;
    std::atomic_bool retired{false};
    mutable std::mutex io_mutex;
    mutable HANDLE completion_port = nullptr;
    NativeFile(std::shared_ptr<const BudgetReservation> reservation, HANDLE handle,
               FileId id, std::uint64_t size, std::uint32_t align, bool async)
        : reservation_(std::move(reservation)), handle_(handle),
          id_(id), size_(size), alignment(align), overlapped(async) {}
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
    FileId id;
    std::shared_ptr<const BudgetReservation> reservation;
    std::shared_ptr<NativeFile> file;
    std::weak_ptr<NativeFile> retired_file;
    bool retired = false;
};

class Registry final : public FileRegistry {
    std::shared_ptr<ResourceGovernor> governor_;
    SynchronousStorageOptions options_;
    std::function<void(FileId)> on_retire_;
    std::shared_ptr<const BudgetReservation> reservation_;
    std::mutex mutex_;
    std::map<FileIdentity, FileEntry> entries_;
public:
    Registry(std::shared_ptr<ResourceGovernor> governor,
             SynchronousStorageOptions options,
             std::function<void(FileId)> on_retire,
             std::shared_ptr<const BudgetReservation> reservation)
        : governor_(std::move(governor)), options_(options),
          on_retire_(std::move(on_retire)),
          reservation_(std::move(reservation)) {}
    Result<std::shared_ptr<const FileLease>> Open(
        const std::filesystem::path& path) override {
        try {
            // No write/delete sharing: immutable data cannot change under a lease.
            Handle handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING,
                (options_.direct_reads ? FILE_FLAG_NO_BUFFERING : FILE_ATTRIBUTE_NORMAL) |
                    (options_.overlapped ? FILE_FLAG_OVERLAPPED : 0), nullptr));
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
            auto charge = std::get<std::shared_ptr<const BudgetReservation>>(std::move(reserved));
            auto file = std::make_shared<NativeFile>(
                charge,
                handle.Get(), std::get<FileId>(id),
                static_cast<std::uint64_t>(size.QuadPart), alignment, options_.overlapped);
            handle.Release();
            entries_.emplace(key, FileEntry{file->Id(), std::move(charge), file, {}, false});
            return std::shared_ptr<const FileLease>(std::move(file));
        } catch (const std::bad_alloc&) {
            return Exhausted();
        }
    }
    Result<Success> Retire(FileId id) override {
        {
            std::lock_guard lock(mutex_);
            auto found = std::find_if(entries_.begin(), entries_.end(), [id](const auto& item) {
                return item.second.id == id;
            });
            if (found == entries_.end()) return Error{ErrorCode::not_found, "Unknown file ID"};
            if (!found->second.retired) {
                found->second.retired = true;
                found->second.file->retired = true;
                found->second.retired_file = found->second.file;
                found->second.file.reset();
            }
        }
        if (on_retire_) on_retire_(id);
        return Success{};
    }
    Result<Success> CollectRetired() override {
        // A retired entry is only dropped once nothing pins the file any more:
        // the caller owns the path, so releasing the entry is what makes the
        // native handle go away and the directory deletable. Entries that a
        // live lease still pins stay registered and are collected on a later
        // call, which is why this returns Success instead of failing.
        std::lock_guard lock(mutex_);
        for (auto item = entries_.begin(); item != entries_.end();) {
            if (item->second.retired && item->second.retired_file.expired())
                item = entries_.erase(item);
            else
                ++item;
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
        if (!callback || !file || file->overlapped || !request.buffer ||
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

class IocpBackend final : public MeteredReadBackend {
    struct Operation {
        OVERLAPPED overlapped{};
        ReadRequest request{};
        ReadCallback callback;
    };
    std::shared_ptr<const BudgetReservation> reservation_;
    std::vector<Operation> slots_;
    Handle port_;
    mutable std::mutex mutex_;
    std::mutex drain_mutex_;
    std::condition_variable drained_;
    ReadBackendStats stats_;
    bool closed_ = false;
    std::thread worker_;
    const ReadRequest* completing_ = nullptr;

    void Complete() {
        const HANDLE port = port_.Get();
        for (;;) {
            DWORD bytes = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* overlapped = nullptr;
            const BOOL ok = GetQueuedCompletionStatus(port, &bytes, &key, &overlapped, INFINITE);
            DWORD error = ok ? ERROR_SUCCESS : GetLastError();
            if (!overlapped) return;

            Operation done;
            {
                std::lock_guard lock(mutex_);
                auto slot = std::find_if(slots_.begin(), slots_.end(), [overlapped](auto& item) {
                    return &item.overlapped == overlapped;
                });
                if (slot == slots_.end() || !slot->callback) std::terminate();
                done = std::move(*slot);
                *slot = {};
                if (ok && bytes != done.request.block.byte_count) error = ERROR_HANDLE_EOF;
                stats_.io_read_bytes_completed += bytes;
                stats_.io_read_errors += error != ERROR_SUCCESS;
                --stats_.inflight_reads;
                completing_ = &done.request;
                drained_.notify_all();
            }
            // The packet is dequeued. Keep the request leases on this thread
            // until the callback returns, even if that callback admits new I/O.
            if (error != ERROR_SUCCESS)
                done.callback(SystemError("IOCP read failed or short", error));
            else
                done.callback(ReadCompletion{done.request.request_id, bytes});
            {
                std::lock_guard lock(mutex_);
                completing_ = nullptr;
            }
        }
    }

public:
    IocpBackend(HANDLE port, std::uint32_t capacity,
                std::shared_ptr<const BudgetReservation> reservation)
        : reservation_(std::move(reservation)), slots_(capacity), port_(port) {}

    void Start() { worker_ = std::thread([this] { Complete(); }); }
    ~IocpBackend() override { static_cast<void>(Drain()); }

    Result<Success> Submit(ReadRequest request, ReadCallback callback) override {
        const auto* file = dynamic_cast<const NativeFile*>(request.file.get());
        const auto& block = request.block;
        const auto buffer = request.buffer ? request.buffer->WritableBytes() : std::span<std::byte>{};
        const auto address = reinterpret_cast<std::uintptr_t>(buffer.data());
        if (!callback || !file || !file->overlapped || !address ||
            block.file_id != file->Id() || !block.byte_count ||
            block.aligned_offset > file->SizeBytes() ||
            block.byte_count > file->SizeBytes() - block.aligned_offset ||
            block.byte_count > buffer.size() || address % file->alignment ||
            block.byte_count % file->alignment || block.aligned_offset % file->alignment ||
            (request.priority != ReadPriority::demand &&
             request.priority != ReadPriority::prefetch &&
             request.priority != ReadPriority::background))
            return Invalid("Invalid IOCP read request");
        std::lock_guard lock(mutex_);
        if (closed_) return Error{ErrorCode::closed, "IOCP backend drained"};
        const auto conflicts = [&](const ReadRequest& active) {
            const auto other = reinterpret_cast<std::uintptr_t>(
                active.buffer->WritableBytes().data());
            const auto size = active.buffer->WritableBytes().size();
            return active.request_id == request.request_id ||
                (address >= other ? address - other < size : other - address < buffer.size());
        };
        if (completing_ && conflicts(*completing_))
            return Invalid("Duplicate completing ID or overlapping buffer");
        Operation* slot = nullptr;
        for (auto& active : slots_) {
            if (!active.callback) {
                if (!slot) slot = &active;
                continue;
            }
            if (conflicts(active.request))
                return Invalid("Duplicate in-flight ID or overlapping buffer");
        }
        if (!slot) return Exhausted();
        {
            std::lock_guard file_lock(file->io_mutex);
            if (!file->completion_port) {
                if (!CreateIoCompletionPort(file->NativeHandle(), port_.Get(), 0, 0))
                    return SystemError("Associate file with IOCP", GetLastError());
                file->completion_port = port_.Get();
            } else if (file->completion_port != port_.Get()) {
                return Invalid("File belongs to another IOCP backend");
            }
        }
        slot->overlapped.Offset = static_cast<DWORD>(block.aligned_offset);
        slot->overlapped.OffsetHigh = static_cast<DWORD>(block.aligned_offset >> 32);
        const DWORD length = block.byte_count;
        slot->request = std::move(request);
        slot->callback = std::move(callback);
        ++stats_.io_reads_submitted;
        if (!ReadFile(file->NativeHandle(), buffer.data(), length, nullptr, &slot->overlapped)) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                *slot = {};
                ++stats_.io_read_errors;
                return SystemError("Submit IOCP ReadFile", error);
            }
        }
        // Immediate success also produces a completion packet.
        ++stats_.inflight_reads;
        return Success{};
    }

    Result<Success> Drain() override {
        std::lock_guard drain_lock(drain_mutex_);
        {
            std::unique_lock lock(mutex_);
            closed_ = true;
            drained_.wait(lock, [&] { return stats_.inflight_reads == 0; });
        }
        if (worker_.joinable()) {
            if (!PostQueuedCompletionStatus(port_.Get(), 0, 0, nullptr)) {
                const DWORD error = GetLastError();
                CloseHandle(port_.Release());
                worker_.join();
                return SystemError("Stop IOCP worker", error);
            }
            worker_.join();
        }
        return Success{};
    }

    ReadBackendStats Stats() const override {
        std::lock_guard lock(mutex_);
        return stats_;
    }

    static std::uint64_t StorageBytes(std::uint32_t capacity) {
        return sizeof(IocpBackend) + sizeof(Operation) * std::uint64_t{capacity} + 256;
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

std::uint64_t MemoryAccountedBytes(const ResourceGovernor& governor) {
    const auto* native = dynamic_cast<const Governor*>(&governor);
    return native ? native->AccountedBytes() : 0;
}

Result<std::unique_ptr<FileRegistry>> CreateSynchronousFileRegistry(
    std::shared_ptr<ResourceGovernor> governor, const SynchronousStorageOptions& options,
    std::function<void(FileId)> on_retire) {
    if (!governor || !options.max_registered_files) return Invalid("Invalid registry options");
    auto reserved = governor->TryReserve({MemoryRegion::metadata, sizeof(Registry) + 256});
    if (const auto* error = std::get_if<Error>(&reserved)) return *error;
    try {
        return std::unique_ptr<FileRegistry>(std::make_unique<Registry>(
            std::move(governor), options, std::move(on_retire),
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

bool ReadFileRetired(const FileLease& file) noexcept {
    const auto* native = dynamic_cast<const NativeFile*>(&file);
    return native && native->retired.load();
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

Result<std::unique_ptr<MeteredReadBackend>> CreateIocpReadBackend(
    std::shared_ptr<ResourceGovernor> governor, std::uint32_t capacity) {
    if (!governor || !capacity) return Invalid("Invalid IOCP read capacity");
    auto reserved = governor->TryReserve({MemoryRegion::metadata, IocpBackend::StorageBytes(capacity)});
    if (const auto* error = std::get_if<Error>(&reserved)) return *error;
    const auto native = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (!native) return SystemError("Create IOCP", GetLastError());
    Handle port(native);
    try {
        auto backend = std::make_unique<IocpBackend>(port.Get(), capacity,
            std::get<std::shared_ptr<const BudgetReservation>>(std::move(reserved)));
        port.Release();
        backend->Start();
        return std::unique_ptr<MeteredReadBackend>(std::move(backend));
    } catch (const std::bad_alloc&) {
        return Exhausted();
    } catch (const std::system_error&) {
        return Error{ErrorCode::resource_exhausted, "Cannot start IOCP completion worker"};
    }
}

}  // namespace agent_io
