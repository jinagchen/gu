#include "agent_io/synchronous_storage.hpp"
#include "pq_flash_index.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
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

template<class T> T Take(Result<T> result) {
    if (const auto* error = std::get_if<Error>(&result))
        throw std::runtime_error(error->message + "; system_error=" +
                                 std::to_string(error->system_error));
    return std::get<T>(std::move(result));
}

class ProbeReader final : public AlignedFileReader {
    IOContext context_;
    // Only this adapter's storage allocations are governed here. DiskANN PQ,
    // scratch and the process still require the external Job measurements.
    std::shared_ptr<ResourceGovernor> governor_ = Take(CreateResourceGovernor(
        {16777216, 2097152, 2097152, 2097152, 8388608, 2097152}));
    std::unique_ptr<FileRegistry> registry_ = Take(CreateSynchronousFileRegistry(governor_));
    std::shared_ptr<SynchronousReadBackend> native_backend_ =
        Take(CreateSynchronousReadBackend(governor_));
    std::unique_ptr<BlockReader> blocks_ =
        Take(CreateSynchronousBlockReader(governor_, native_backend_));
    std::shared_ptr<const FileLease> file_;
public:
    struct Counters {
        std::uint64_t reads = 0, bytes = 0;
        bool fail = false;
    } backend;
    IOContext& get_ctx() override { return context_; }
    void register_thread() override {}
    void deregister_thread() override {}
    void deregister_all_threads() override {}
    void open(const std::string& path) override {
        file_ = Take(registry_->Open(path));
        std::fprintf(stderr, "probe_reader_open=pass\n");
    }
    void close() override { file_.reset(); }
    void read(std::vector<AlignedRead>& requests, IOContext&, bool = false) override {
        if (!file_) throw std::runtime_error("Read before graph open");
        if (backend.fail) throw std::runtime_error("Injected reader submission failure");
        for (std::size_t offset = 0; offset < requests.size(); offset += 64) {
            const auto count = (std::min)(std::size_t{64}, requests.size() - offset);
            std::vector<BlockLocation> locations;
            locations.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                const auto& request = requests[offset + i];
                if (request.len > (std::numeric_limits<std::uint32_t>::max)())
                    throw std::runtime_error("Read exceeds DWORD");
                locations.push_back({{file_->Id(), request.offset,
                    static_cast<std::uint32_t>(request.len)}, file_});
            }
            const auto leases = Take(blocks_->ReadBatch(locations,
                {ReadPriority::demand, std::chrono::steady_clock::now() +
                    std::chrono::seconds(30)}));
            for (std::size_t i = 0; i < count; ++i)
                std::memcpy(requests[offset + i].buf, leases[i]->Bytes().data(),
                            leases[i]->Bytes().size());
            const auto stats = native_backend_->Stats();
            backend.reads = stats.io_reads_submitted;
            backend.bytes = stats.io_read_bytes_completed;
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
        std::puts("storage_implementation=agent_io_storage; block_reader=cache_disabled");
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
