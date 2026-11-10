#include "agent_io/storage.hpp"
#include "pq_flash_index.h"

#include <windows.h>
#include <psapi.h>

#include <cstdio>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {

using namespace agent_io;

std::size_t PrivateBytes() {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
        throw std::runtime_error("GetProcessMemoryInfo failed");
    return memory.PrivateUsage;
}

class ProbeFile final : public FileLease {
public:
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::uint64_t size = 0;
    explicit ProbeFile(const std::string& path) {
        handle = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Open graph failed");
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(handle, &length)) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
            throw std::runtime_error("Graph size failed");
        }
        size = static_cast<std::uint64_t>(length.QuadPart);
    }
    ~ProbeFile() override { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    FileId Id() const noexcept override { return 1; }
    std::uint64_t SizeBytes() const noexcept override { return size; }
};

// Borrowed DiskANN storage is safe here because Submit completes synchronously.
class ProbeBuffer final : public BufferLease {
    std::span<std::byte> bytes_;
public:
    ProbeBuffer(void* bytes, std::size_t count)
        : bytes_(static_cast<std::byte*>(bytes), count) {}
    std::span<std::byte> WritableBytes() noexcept override { return bytes_; }
    std::span<const std::byte> Bytes() const noexcept override { return bytes_; }
};

class ProbeBackend final : public ReadBackend {
    std::mutex mutex_;
public:
    std::uint64_t reads = 0, bytes = 0;
    bool fail = false;
    Result<Success> Submit(ReadRequest request, ReadCallback callback) override {
        const auto file = std::dynamic_pointer_cast<const ProbeFile>(request.file);
        if (!file || !request.buffer || request.buffer->Bytes().size() < request.block.byte_count)
            return Error{ErrorCode::invalid_argument, "Invalid probe request"};
        if (fail) return Error{ErrorCode::io_error, "Injected reader submission failure"};
        DWORD completed = 0;
        LARGE_INTEGER offset{};
        offset.QuadPart = static_cast<LONGLONG>(request.block.aligned_offset);
        bool success;
        DWORD error;
        {
            std::lock_guard lock(mutex_);
            success = SetFilePointerEx(file->handle, offset, nullptr, FILE_BEGIN) &&
                ReadFile(file->handle, request.buffer->WritableBytes().data(),
                         request.block.byte_count, &completed, nullptr);
            error = GetLastError();
            ++reads;
            bytes += completed;
        }
        if (!success || completed != request.block.byte_count) {
            callback(Error{ErrorCode::io_error, "Graph read failed or short",
                           static_cast<int>(error)});
        } else {
            callback(ReadCompletion{request.request_id, completed});
        }
        return Success{};
    }
    Result<Success> Drain() override { return Success{}; }
};

class ProbeReader final : public AlignedFileReader {
    IOContext context_;
    std::shared_ptr<ProbeFile> file_;
    RequestId next_ = 0;
public:
    ProbeBackend backend;
    IOContext& get_ctx() override { return context_; }
    void register_thread() override {}
    void deregister_thread() override {}
    void deregister_all_threads() override {}
    void open(const std::string& path) override {
        file_ = std::make_shared<ProbeFile>(path);
        std::fprintf(stderr, "probe_reader_open=pass\n");
    }
    void close() override { file_.reset(); }
    void read(std::vector<AlignedRead>& requests, IOContext&, bool = false) override {
        for (const auto& request : requests) {
            if (request.len > (std::numeric_limits<std::uint32_t>::max)())
                throw std::runtime_error("Read exceeds DWORD");
            ReadRequest read{
                ++next_, {1, request.offset, static_cast<std::uint32_t>(request.len)},
                ReadPriority::demand, file_,
                std::make_shared<ProbeBuffer>(request.buf, static_cast<std::size_t>(request.len))};
            bool completed = false;
            std::string failure;
            auto submitted = backend.Submit(std::move(read), [&](Result<ReadCompletion> result) {
                completed = true;
                if (auto* error = std::get_if<Error>(&result)) failure = error->message;
            });
            if (auto* error = std::get_if<Error>(&submitted)) throw std::runtime_error(error->message);
            if (!completed || !failure.empty()) throw std::runtime_error("Read completion: " + failure);
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 4 && argc != 5)
            throw std::runtime_error("Usage: diskann_probe prefix queries.bin results.jsonl [fail]");
        const auto initial_private = PrivateBytes();
        auto concrete = std::make_shared<ProbeReader>();
        std::shared_ptr<AlignedFileReader> reader = concrete;
        diskann::PQFlashIndex<float> index(reader, diskann::Metric::L2);
        if (index.load(1, argv[1]) != 0) throw std::runtime_error("Index load failed");
        std::printf("initial_private_bytes=%zu; loaded_private_bytes=%zu\n",
                    initial_private, PrivateBytes());
        std::ifstream queries(argv[2], std::ios::binary);
        std::uint32_t count = 0, dimension = 0;
        queries.read(reinterpret_cast<char*>(&count), sizeof(count));
        queries.read(reinterpret_cast<char*>(&dimension), sizeof(dimension));
        if (!queries || dimension != index.get_data_dim() || count == 0)
            throw std::runtime_error("Invalid query header");
        const auto before_reads = concrete->backend.reads;
        const auto before_bytes = concrete->backend.bytes;
        std::ofstream output(argv[3]);
        if (!output) throw std::runtime_error("Open output failed");
        std::vector<float> query(dimension);
        std::uint64_t ids[10]{};
        float distances[10]{};
        concrete->backend.fail = argc == 5 && std::string(argv[4]) == "fail";
        for (std::uint32_t i = 0; i < count; ++i) {
            queries.read(reinterpret_cast<char*>(query.data()), dimension * sizeof(float));
            if (!queries) throw std::runtime_error("Truncated queries");
            index.cached_beam_search(query.data(), 10, 128, ids, distances, 4);
            output << "{\"query_id\":" << i << ",\"ids\":[";
            for (int j = 0; j < 10; ++j) output << (j ? "," : "") << ids[j];
            output << "]}\n";
        }
        output.flush();
        if (!output) throw std::runtime_error("Write output failed");
        if (concrete->backend.reads == before_reads)
            throw std::runtime_error("No query graph reads observed");
        std::printf("query_graph_reads=%llu; query_graph_bytes=%llu; queries=%u\n",
                    static_cast<unsigned long long>(concrete->backend.reads - before_reads),
                    static_cast<unsigned long long>(concrete->backend.bytes - before_bytes), count);
        std::puts("read_path=DiskANN_AlignedFileReader_to_agent_io_ReadBackend; direct_reads=true");
        if (argc == 5 && std::string(argv[4]) == "hold") {
            const std::string ready = std::string(argv[3]) + ".ready";
            const std::string release = std::string(argv[3]) + ".release";
            std::ofstream(ready).put('1');
            const auto deadline = GetTickCount64() + 60000;
            while (!std::filesystem::exists(release)) {
                if (GetTickCount64() > deadline) throw std::runtime_error("Hold timed out");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "diskann_probe_failed: %s\n", error.what());
        return 1;
    }
}
