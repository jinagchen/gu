#include <windows.h>
#include <malloc.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class Handle {
public:
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() {
        if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) {
            CloseHandle(value_);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const { return value_; }

private:
    HANDLE value_;
};

void Require(bool condition, const char* operation) {
    if (!condition) {
        throw std::runtime_error(
            std::string(operation) + "; GetLastError=" +
            std::to_string(GetLastError()));
    }
}

class TemporaryFile {
public:
    explicit TemporaryFile(const wchar_t* selected_directory) {
        wchar_t directory[MAX_PATH + 1]{};
        if (selected_directory == nullptr) {
            const DWORD length = GetTempPathW(MAX_PATH, directory);
            Require(length > 0 && length < MAX_PATH, "GetTempPathW");
            selected_directory = directory;
        }
        Require(GetTempFileNameW(selected_directory, L"agi", 0, path_) != 0,
                "GetTempFileNameW");
    }
    ~TemporaryFile() { DeleteFileW(path_); }
    const wchar_t* Path() const { return path_; }

private:
    wchar_t path_[MAX_PATH + 1]{};
};

struct AlignedFree {
    void operator()(std::byte* value) const { _aligned_free(value); }
};

using Buffer = std::unique_ptr<std::byte, AlignedFree>;

Buffer Allocate(DWORD size, DWORD alignment) {
    Buffer result(static_cast<std::byte*>(_aligned_malloc(size, alignment)));
    if (!result) {
        throw std::runtime_error("_aligned_malloc");
    }
    return result;
}

void CheckBytes(const std::byte* bytes, const std::vector<std::byte>& expected,
                std::size_t offset, std::size_t count) {
    Require(std::equal(bytes, bytes + count, expected.data() + offset),
            "Read content mismatch");
}

void ProbeFileIo(const wchar_t* directory) {
    // This probe touches only its unique temporary file, never dataset files.
    TemporaryFile temporary(directory);
    DWORD alignment = 0;
    DWORD block = 0;
    std::vector<std::byte> expected;
    {
        Handle writer(CreateFileW(temporary.Path(), GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr));
        Require(writer.Get() != INVALID_HANDLE_VALUE, "CreateFileW writer");
        FILE_STORAGE_INFO storage{};
        Require(GetFileInformationByHandleEx(writer.Get(), FileStorageInfo,
                                             &storage, sizeof(storage)) != 0,
                "FileStorageInfo");
        alignment = (std::max)({
            storage.LogicalBytesPerSector,
            storage.PhysicalBytesPerSectorForAtomicity,
            storage.PhysicalBytesPerSectorForPerformance,
            static_cast<DWORD>(sizeof(void*))});
        Require(alignment != 0 && (alignment & (alignment - 1)) == 0 &&
                    alignment <= 1024 * 1024,
                "Unsupported sector alignment");
        block = (std::max)(DWORD{4096}, alignment);
        expected.resize(static_cast<std::size_t>(block) * 2);
        for (std::size_t i = 0; i < expected.size(); ++i) {
            expected[i] = static_cast<std::byte>((i * 17 + i / block) % 251);
        }
        DWORD written = 0;
        Require(WriteFile(writer.Get(), expected.data(),
                          static_cast<DWORD>(expected.size()), &written,
                          nullptr) != 0 &&
                    written == static_cast<DWORD>(expected.size()),
                "WriteFile");
        Require(FlushFileBuffers(writer.Get()) != 0, "FlushFileBuffers");
        std::printf("file_flush=pass; logical_sector=%lu; alignment=%lu\n",
                    storage.LogicalBytesPerSector, alignment);
    }
    {
        Handle reader(CreateFileW(temporary.Path(), GENERIC_READ, 0, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_NO_BUFFERING,
                                  nullptr));
        Require(reader.Get() != INVALID_HANDLE_VALUE,
                "CreateFileW synchronous unbuffered");
        auto buffer = Allocate(block, alignment);
        DWORD completed = 0;
        Require(ReadFile(reader.Get(), buffer.get(), block, &completed,
                         nullptr) != 0 && completed == block,
                "ReadFile synchronous unbuffered");
        CheckBytes(buffer.get(), expected, 0, block);
        std::printf("synchronous_unbuffered_read=pass; bytes=%lu\n", completed);
        LARGE_INTEGER end{};
        end.QuadPart = static_cast<LONGLONG>(block) * 2;
        Require(SetFilePointerEx(reader.Get(), end, nullptr, FILE_BEGIN) != 0,
                "Seek to EOF");
        completed = block;
        Require(ReadFile(reader.Get(), buffer.get(), block, &completed,
                         nullptr) != 0 && completed == 0,
                "Synchronous EOF short read");
        std::puts("synchronous_eof_short_read=pass; bytes=0");
    }
    {
        Handle reader(CreateFileW(
            temporary.Path(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED, nullptr));
        Require(reader.Get() != INVALID_HANDLE_VALUE,
                "CreateFileW overlapped unbuffered");
        Handle port(CreateIoCompletionPort(reader.Get(), nullptr, 7, 1));
        Require(port.Get() != nullptr, "CreateIoCompletionPort");
        auto first = Allocate(block, alignment);
        auto second = Allocate(block, alignment);
        OVERLAPPED requests[2]{};
        requests[1].Offset = block;
        std::byte* buffers[] = {first.get(), second.get()};
        DWORD accepted = 0;
        bool submission_ok = true;
        DWORD submission_error = ERROR_SUCCESS;
        for (DWORD i = 0; i < 2; ++i) {
            const BOOL result = ReadFile(reader.Get(), buffers[i], block,
                                         nullptr, &requests[i]);
            if (!result && GetLastError() != ERROR_IO_PENDING) {
                submission_ok = false;
                submission_error = GetLastError();
                break;
            }
            ++accepted;
        }
        bool completion_ok = true;
        bool seen[2]{};
        // Even after an error, retain every buffer until accepted I/O drains.
        for (DWORD i = 0; i < accepted; ++i) {
            DWORD completed = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* request = nullptr;
            const BOOL result = GetQueuedCompletionStatus(
                port.Get(), &completed, &key, &request, INFINITE);
            const int index = request == &requests[0] ? 0 :
                              request == &requests[1] ? 1 : -1;
            if (!result || index < 0 || completed != block || key != 7 ||
                (index >= 0 && seen[index])) {
                completion_ok = false;
                continue;
            }
            seen[index] = true;
        }
        if (!submission_ok) {
            SetLastError(submission_error);
        }
        Require(submission_ok && completion_ok && seen[0] && seen[1],
                "IOCP submissions and completions");
        CheckBytes(first.get(), expected, 0, block);
        CheckBytes(second.get(), expected, block, block);
        std::printf("iocp_unbuffered_reads=pass; requests=2; bytes=%lu\n",
                    block * 2);

        OVERLAPPED eof{};
        eof.Offset = block * 2;
        const BOOL submitted = ReadFile(reader.Get(), first.get(), block,
                                       nullptr, &eof);
        Require(submitted || GetLastError() == ERROR_IO_PENDING,
                "Submit asynchronous EOF");
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* done = nullptr;
        const BOOL result = GetQueuedCompletionStatus(
            port.Get(), &bytes, &key, &done, 5000);
        const DWORD error = GetLastError();
        Require(!result && done == &eof && error == ERROR_HANDLE_EOF &&
                    bytes == 0, "Asynchronous EOF completion");
        std::puts("iocp_eof_error=pass");

        OVERLAPPED invalid{};
        const BOOL invalid_result = ReadFile(reader.Get(), first.get(),
                                            block - 1, nullptr, &invalid);
        const DWORD invalid_error = GetLastError();
        // Some drivers defer alignment errors to completion.
        if (invalid_result || invalid_error == ERROR_IO_PENDING) {
            done = nullptr;
            const BOOL invalid_done = GetQueuedCompletionStatus(
                port.Get(), &bytes, &key, &done, 5000);
            Require(!invalid_done && done == &invalid &&
                        GetLastError() == ERROR_INVALID_PARAMETER,
                    "Deferred unaligned submission failure");
        } else {
            Require(invalid_error == ERROR_INVALID_PARAMETER,
                    "Immediate unaligned submission failure");
        }
        std::puts("unaligned_read_rejected=pass");

        OVERLAPPED cancel{};
        const BOOL cancel_submitted = ReadFile(reader.Get(), first.get(), block,
                                              nullptr, &cancel);
        Require(cancel_submitted || GetLastError() == ERROR_IO_PENDING,
                "Submit cancellation probe");
        const BOOL cancelled = CancelIoEx(reader.Get(), &cancel);
        const DWORD cancel_error = GetLastError();
        // Cancellation may race a successful read; either completion must drain.
        done = nullptr;
        const BOOL drained = GetQueuedCompletionStatus(
            port.Get(), &bytes, &key, &done, 5000);
        const DWORD drain_error = GetLastError();
        Require(done == &cancel &&
                    (drained || drain_error == ERROR_OPERATION_ABORTED) &&
                    (cancelled || cancel_error == ERROR_NOT_FOUND),
                "Cancellation completion drain");
        std::puts("cancel_and_drain=pass");
    }
}

void ProbeMemoryLimit() {
    Handle job(CreateJobObjectW(nullptr, nullptr));
    Require(job.Get() != nullptr, "CreateJobObjectW");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_JOB_MEMORY;
    limits.JobMemoryLimit = SIZE_T{64} * 1024 * 1024;
    Require(SetInformationJobObject(job.Get(),
                                   JobObjectExtendedLimitInformation,
                                   &limits, sizeof(limits)) != 0,
            "SetInformationJobObject");
    Require(AssignProcessToJobObject(job.Get(), GetCurrentProcess()) != 0,
            "AssignProcessToJobObject probe only");
    void* small = VirtualAlloc(nullptr, SIZE_T{8} * 1024 * 1024,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    Require(small != nullptr, "Allocation within job limit");
    VirtualFree(small, 0, MEM_RELEASE);
    void* large = VirtualAlloc(nullptr, SIZE_T{96} * 1024 * 1024,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    const DWORD rejection = GetLastError();
    if (large != nullptr) {
        VirtualFree(large, 0, MEM_RELEASE);
        throw std::runtime_error("Allocation exceeded job memory limit");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION measured{};
    Require(QueryInformationJobObject(job.Get(),
                                     JobObjectExtendedLimitInformation,
                                     &measured, sizeof(measured), nullptr) != 0,
            "QueryInformationJobObject");
    std::printf(
        "job_commit_limit=pass; limit_bytes=%llu; rejected_bytes=%llu; "
        "error=%lu; peak_job_commit_bytes=%llu\n",
        static_cast<unsigned long long>(limits.JobMemoryLimit),
        96ULL * 1024 * 1024, rejection,
        static_cast<unsigned long long>(measured.PeakJobMemoryUsed));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        Require(argc == 1 || argc == 2, "Usage: windows_environment_probe [directory]");
        ProbeFileIo(argc == 2 ? argv[1] : nullptr);
        ProbeMemoryLimit();
        std::puts("scope=functionality_only; engine_not_implemented");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "probe_failed: %s\n", error.what());
        return 1;
    }
}
