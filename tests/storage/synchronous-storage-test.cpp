#include "agent_io/synchronous_storage.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <future>
#include <source_location>
#include <stdexcept>
#include <thread>

namespace {
using namespace agent_io;
using namespace std::chrono_literals;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class T> T Take(Result<T> result) {
    if (const auto* error = std::get_if<Error>(&result))
        throw std::runtime_error(error->message + ": " + std::to_string(error->system_error));
    return std::get<T>(std::move(result));
}

template<class T> void Expect(Result<T> result, ErrorCode code,
    std::source_location location = std::source_location::current()) {
    const auto* error = std::get_if<Error>(&result);
    if (!error || error->code != code)
        throw std::runtime_error("Unexpected error result at line " +
            std::to_string(location.line()) + (error ? ": " + error->message : ": success"));
}

MemoryBudget Budget() {
    return {16 * 1024 * 1024, 2 * 1024 * 1024, 2 * 1024 * 1024,
            2 * 1024 * 1024, 8 * 1024 * 1024, 2 * 1024 * 1024};
}

struct Fixture {
    std::filesystem::path path, alias;
    std::vector<std::byte> expected;
    Fixture() {
        wchar_t directory[MAX_PATH]{}, name[MAX_PATH]{};
        Require(GetTempPathW(MAX_PATH, directory) != 0, "Temp directory");
        Require(GetTempFileNameW(directory, L"aio", 0, name) != 0, "Temp filename");
        path = name;
        alias = path;
        alias += L".link";
        expected.resize(4 * 1024 * 1024);
        for (std::size_t i = 0; i < expected.size(); ++i)
            expected[i] = static_cast<std::byte>((i * 17 + i / 4096) % 251);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(expected.data()),
                     static_cast<std::streamsize>(expected.size()));
        output.close();
        Require(static_cast<bool>(output), "Fixture write");
        Require(CreateHardLinkW(alias.c_str(), path.c_str(), nullptr) != 0, "Hard link");
    }
    ~Fixture() {
        DeleteFileW(alias.c_str());
        DeleteFileW(path.c_str());
    }
};

class ReadOnlyBuffer final : public BufferLease {
    std::byte* memory_ = static_cast<std::byte*>(
        VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READONLY));
public:
    ReadOnlyBuffer() { Require(memory_ != nullptr, "Read-only buffer allocation"); }
    ~ReadOnlyBuffer() override { VirtualFree(memory_, 0, MEM_RELEASE); }
    std::span<std::byte> WritableBytes() noexcept override { return {memory_, 4096}; }
    std::span<const std::byte> Bytes() const noexcept override { return {memory_, 4096}; }
};

void CheckBudgets() {
    auto query = Budget();
    auto rebuild = query;
    rebuild.workspace_bytes /= 2;
    rebuild.cache_bytes += query.workspace_bytes / 2;
    auto governor = Take(CreateResourceGovernor(query, rebuild));
    Expect(CreateResourceGovernor(MemoryBudget{}), ErrorCode::invalid_argument);
    auto overflow = query;
    overflow.workspace_bytes = UINT64_MAX;
    Expect(CreateResourceGovernor(overflow), ErrorCode::invalid_argument);
    Expect(governor->TryReserve({MemoryRegion::workspace, 0}), ErrorCode::invalid_argument);
    auto token = Take(governor->TryReserve({MemoryRegion::workspace, query.workspace_bytes}));
    auto copy = token;
    token.reset();
    Expect(governor->TryReserve({MemoryRegion::workspace, 1}), ErrorCode::resource_exhausted);
    Expect(governor->TrySwitchMode(BudgetMode::rebuild), ErrorCode::resource_exhausted);
    Require(governor->Mode() == BudgetMode::query, "Failed switch changed mode");
    copy.reset();
    Take(governor->TrySwitchMode(BudgetMode::rebuild));
    token = Take(governor->TryReserve({MemoryRegion::workspace, rebuild.workspace_bytes}));
    Require(token->Charge().bytes == rebuild.workspace_bytes, "Wrong charge");
    governor.reset();
    token.reset();
    governor = Take(CreateResourceGovernor(query));
    Expect(governor->TrySwitchMode(BudgetMode::rebuild), ErrorCode::unsupported);
    auto buffer = Take(AllocateReadBuffer(governor, 4096, 4096));
    auto remaining = Take(governor->TryReserve(
        {MemoryRegion::workspace, query.workspace_bytes - 8192 - 1024}));
    Expect(AllocateReadBuffer(governor, 8192, 4096), ErrorCode::resource_exhausted);
    remaining.reset();
    buffer.reset();
    Take(governor->TryReserve({MemoryRegion::workspace, query.workspace_bytes}));
}

ReadRequest Request(RequestId id, const std::shared_ptr<const FileLease>& file,
                    const std::shared_ptr<BufferLease>& buffer,
                    std::uint64_t offset, std::uint32_t count) {
    return {id, {file->Id(), offset, count}, ReadPriority::demand, file, buffer};
}

void CheckReads(Fixture& fixture, bool direct) {
    auto governor = Take(CreateResourceGovernor(Budget()));
    SynchronousStorageOptions options;
    options.direct_reads = direct;
    auto registry = Take(CreateSynchronousFileRegistry(governor, options));
    auto file = Take(registry->Open(fixture.path));
    auto alias = Take(registry->Open(fixture.alias));
    Require(file->Id() == alias->Id(), "Hard link identity mismatch");
    Require(file->SizeBytes() == fixture.expected.size(), "File size mismatch");
    auto missing = fixture.path;
    missing += L".missing";
    Expect(registry->Open(missing), ErrorCode::not_found);
    const auto alignment = Take(ReadAlignment(*file));
    const auto block = (std::max)(alignment, std::uint32_t{4096});
    auto backend = Take(CreateSynchronousReadBackend(governor, options));
    auto buffer = Take(AllocateReadBuffer(governor, block, alignment));
    unsigned callbacks = 0;
    Take(backend->Submit(Request(1, file, buffer, block, block),
        [&](Result<ReadCompletion> result) {
            const auto* completed = std::get_if<ReadCompletion>(&result);
            Require(completed && completed->request_id == 1 &&
                    completed->bytes_completed == block, "Read completion");
            ++callbacks;
        }));
    Require(callbacks == 1, "Callback count");
    Require(std::equal(buffer->Bytes().begin(), buffer->Bytes().end(),
        fixture.expected.begin() + block), "Read contents");
    const auto reject = [&](ReadRequest request) {
        Expect(backend->Submit(std::move(request),
            [&](Result<ReadCompletion>) { ++callbacks; }), ErrorCode::invalid_argument);
        Require(callbacks == 1, "Rejected request called callback");
    };
    reject(Request(2, file, buffer, file->SizeBytes(), block));
    reject(Request(2, file, buffer, UINT64_MAX, block));
    reject(Request(2, file, buffer, 0, block + 1));
    reject(Request(2, file, buffer, 0, 0));
    auto wrong = Request(2, file, buffer, 0, block);
    ++wrong.block.file_id;
    reject(wrong);
    wrong = Request(2, file, buffer, 0, block);
    wrong.priority = ReadPriority::prefetch;
    reject(wrong);
    wrong.file.reset();
    wrong.buffer.reset();
    if (direct) reject(Request(2, file, buffer, 1, block));
    Expect(backend->Submit(Request(2, file, buffer, 0, block), {}),
           ErrorCode::invalid_argument);

    // A shared native file pointer must remain safe across independent backends.
    auto second = Take(CreateSynchronousReadBackend(governor));
    std::atomic<bool> correct{true};
    const auto run = [&](SynchronousReadBackend& reader, std::uint64_t offset) {
        auto local = Take(AllocateReadBuffer(governor, block, alignment));
        for (RequestId i = 0; i < 32; ++i) {
            const auto result = reader.Submit(Request(i + 10, file, local, offset, block),
                [&](Result<ReadCompletion> done) {
                    if (!std::holds_alternative<ReadCompletion>(done) ||
                        !std::equal(local->Bytes().begin(), local->Bytes().end(),
                                    fixture.expected.begin() + offset))
                        correct = false;
                });
            if (!std::holds_alternative<Success>(result)) correct = false;
        }
    };
    std::thread first_thread(run, std::ref(*backend), 0);
    std::thread second_thread(run, std::ref(*second), block);
    first_thread.join();
    second_thread.join();
    Require(correct, "Concurrent read contents");
    auto stats = backend->Stats();
    Require(stats.io_reads_submitted == 33 &&
            stats.io_read_bytes_completed == static_cast<std::uint64_t>(block) * 33 &&
            stats.io_read_errors == 0 && stats.inflight_reads == 0, "Read accounting");
    Take(registry->Retire(file->Id()));
    Take(registry->Retire(file->Id()));
    Expect(registry->Retire(UINT64_MAX), ErrorCode::not_found);
    Expect(registry->Open(fixture.alias), ErrorCode::closed);
    Take(registry->CollectRetired());
    Take(backend->Submit(Request(99, file, buffer, 0, block),
        [](Result<ReadCompletion> done) { Take(std::move(done)); }));
    alias.reset();
    file.reset();
    Expect(registry->CollectRetired(), ErrorCode::unsupported);
    Require(std::filesystem::exists(fixture.path), "Retirement deleted file");
    Take(backend->Drain());
    Take(backend->Drain());
    Take(second->Drain());
    std::printf("PASS: %s reads, aliases, validation, concurrency, retirement, counters\n",
                direct ? "direct" : "buffered");
}

void CheckDrain(Fixture& fixture) {
    auto governor = Take(CreateResourceGovernor(Budget()));
    SynchronousStorageOptions options;
    options.direct_reads = false;
    options.max_inflight_reads = 1;
    auto registry = Take(CreateSynchronousFileRegistry(governor, options));
    auto file = Take(registry->Open(fixture.path));
    auto buffer = Take(AllocateReadBuffer(governor, 4096, 4096));
    auto other_buffer = Take(AllocateReadBuffer(governor, 4096, 4096));
    auto backend = Take(CreateSynchronousReadBackend(governor, options));
    std::promise<void> entered, release;
    auto resume = release.get_future().share();
    auto submitted = std::async(std::launch::async, [&] {
        return backend->Submit(Request(1, file, buffer, 0, 4096),
            [&](Result<ReadCompletion> result) {
                Take(std::move(result));
                entered.set_value();
                resume.wait();
            });
    });
    entered.get_future().wait();
    // Always release the blocked callback, including when a check throws.
    try {
        Expect(backend->Submit(Request(2, file, buffer, 0, 4096),
            [](Result<ReadCompletion>) {}), ErrorCode::invalid_argument);
        Expect(backend->Submit(Request(1, file, other_buffer, 0, 4096),
            [](Result<ReadCompletion>) {}), ErrorCode::invalid_argument);
        Expect(backend->Submit(Request(2, file, other_buffer, 0, 4096),
            [](Result<ReadCompletion>) {}), ErrorCode::resource_exhausted);
        auto drained = std::async(std::launch::async, [&] { return backend->Drain(); });
        const bool waiting = drained.wait_for(30ms) == std::future_status::timeout;
        release.set_value();
        Take(submitted.get());
        Take(drained.get());
        Require(waiting, "Drain returned before callback");
    } catch (...) {
        try { release.set_value(); } catch (const std::future_error&) {}
        throw;
    }
    Expect(backend->Submit(Request(3, file, buffer, 0, 4096),
        [](Result<ReadCompletion>) {}), ErrorCode::closed);
    registry.reset();
    backend.reset();
    Require(file->SizeBytes() == fixture.expected.size(), "Lease owner lifetime");
    auto failing = Take(CreateSynchronousReadBackend(governor));
    auto protected_buffer = std::make_shared<ReadOnlyBuffer>();
    unsigned failed_callbacks = 0;
    Take(failing->Submit(Request(4, file, protected_buffer, 0, 4096),
        [&](Result<ReadCompletion> result) {
            const auto* error = std::get_if<Error>(&result);
            Require(error && error->code == ErrorCode::io_error &&
                    error->system_error != 0, "Native read failure was hidden");
            ++failed_callbacks;
        }));
    const auto failed_stats = failing->Stats();
    Require(failed_callbacks == 1 && failed_stats.io_reads_submitted == 1 &&
            failed_stats.io_read_errors == 1 && failed_stats.inflight_reads == 0,
            "Native failure accounting");
    Take(failing->Drain());
    std::puts("PASS: callback lifetime, capacity, buffer exclusion, drain, closed admission");
    std::puts("PASS: real ReadFile error, callback exactly once, native error code");
}

class GatedBackend final : public SynchronousReadBackend {
    std::shared_ptr<SynchronousReadBackend> backend_;
public:
    std::promise<void> entered, release;
    std::shared_future<void> resume = release.get_future().share();
    explicit GatedBackend(std::shared_ptr<SynchronousReadBackend> backend)
        : backend_(std::move(backend)) {}
    Result<Success> Submit(ReadRequest request, ReadCallback callback) override {
        return backend_->Submit(std::move(request), [&](Result<ReadCompletion> result) {
            entered.set_value();
            resume.wait();
            callback(std::move(result));
        });
    }
    Result<Success> Drain() override { return backend_->Drain(); }
    ReadBackendStats Stats() const override { return backend_->Stats(); }
};

class ShortBackend final : public SynchronousReadBackend {
public:
    Result<Success> Submit(ReadRequest request, ReadCallback callback) override {
        callback(ReadCompletion{request.request_id, request.block.byte_count - 1});
        return Success{};
    }
    Result<Success> Drain() override { return Success{}; }
    ReadBackendStats Stats() const override { return {}; }
};

void CheckBlocks(Fixture& fixture) {
    auto governor = Take(CreateResourceGovernor(Budget()));
    SynchronousStorageOptions options;
    options.direct_reads = false;
    auto registry = Take(CreateSynchronousFileRegistry(governor, options));
    auto file = Take(registry->Open(fixture.path));
    std::shared_ptr<SynchronousReadBackend> backend =
        Take(CreateSynchronousReadBackend(governor, options));
    auto reader = Take(CreateSynchronousBlockReader(governor, backend, 4));
    const BlockLocation first{{file->Id(), 0, 4096}, file};
    const BlockLocation second{{file->Id(), 4096, 4096}, file};
    const auto demand = [] {
        return BlockReadOptions{ReadPriority::demand, std::chrono::steady_clock::now() + 2s};
    };
    const std::array locations{second, first, second};
    auto blocks = Take(reader->ReadBatch(locations, demand()));
    Require(blocks.size() == 3 && blocks[0] == blocks[2], "Batch ordering/duplicate reuse");
    Require(blocks[0]->Key() == second.key && blocks[1]->Key() == first.key,
            "Block keys");
    Require(std::equal(blocks[0]->Bytes().begin(), blocks[0]->Bytes().end(),
        fixture.expected.begin() + 4096), "Block contents");
    Require(backend->Stats().io_reads_submitted == 2, "Duplicate block submitted twice");
    auto invalid = first;
    invalid.key.byte_count = 0;
    const std::array invalid_batch{first, invalid};
    Expect(reader->ReadBatch(invalid_batch, demand()), ErrorCode::invalid_argument);
    Require(backend->Stats().io_reads_submitted == 2, "Invalid batch partially submitted");
    Expect(reader->ReadBatch(locations,
        {ReadPriority::demand, std::chrono::steady_clock::now() - 1ms}),
        ErrorCode::deadline_exceeded);
    const std::array too_many{first, first, first, first, first};
    Expect(reader->ReadBatch(too_many, demand()), ErrorCode::invalid_argument);
    Require(Take(reader->TryPrefetch(first, demand().deadline)) ==
        PrefetchDisposition::dropped, "Prefetch was not disabled");
    Expect(reader->TryPrefetch(invalid, demand().deadline), ErrorCode::invalid_argument);
    auto background = demand();
    background.priority = ReadPriority::background;
    Expect(reader->ReadBatch(locations, background), ErrorCode::unsupported);
    Require(Take(reader->ReadBatch({}, demand())).empty(), "Empty batch");
    Take(reader->Drain());
    Expect(reader->ReadBatch(locations, demand()), ErrorCode::closed);
    Expect(reader->TryPrefetch(first, demand().deadline), ErrorCode::closed);
    reader.reset();
    registry.reset();
    file.reset();
    Require(blocks[0]->Bytes()[0] == fixture.expected[4096], "Block lease lifetime");
    blocks.clear();

    // The payload for the entire batch must fit before any file is read.
    auto held = Take(governor->TryReserve(
        {MemoryRegion::workspace, Budget().workspace_bytes - 8000}));
    backend = Take(CreateSynchronousReadBackend(governor, options));
    reader = Take(CreateSynchronousBlockReader(governor, backend, 4));
    Expect(reader->ReadBatch(locations, demand()), ErrorCode::resource_exhausted);
    Require(backend->Stats().io_reads_submitted == 0, "Budget failure partially read batch");
    held.reset();
    Take(reader->Drain());

    auto gate = std::make_shared<GatedBackend>(
        Take(CreateSynchronousReadBackend(governor, options)));
    reader = Take(CreateSynchronousBlockReader(governor, gate, 4));
    const std::array single{first};
    auto leader = std::async(std::launch::async, [&] {
        return reader->ReadBatch(single, demand());
    });
    gate->entered.get_future().wait();
    auto follower = std::async(std::launch::async, [&] {
        return reader->ReadBatch(single,
            {ReadPriority::demand, std::chrono::steady_clock::now() + 20ms});
    });
    const bool timely = follower.wait_for(500ms) == std::future_status::ready;
    gate->release.set_value();
    auto leader_blocks = Take(leader.get());
    auto follower_result = follower.get();
    Require(timely, "Shared-read waiter failed to respect deadline");
    Expect(std::move(follower_result), ErrorCode::deadline_exceeded);
    Require(leader_blocks.size() == 1 && gate->Stats().io_reads_submitted == 1,
            "Shared demand performed duplicate I/O");
    Take(reader->Drain());
    reader = Take(CreateSynchronousBlockReader(governor, std::make_shared<ShortBackend>()));
    Expect(reader->ReadBatch(single, demand()), ErrorCode::io_error);
    Take(reader->Drain());
    std::puts("PASS: block batches, duplicate reuse, all-or-error, budgets, lease lifetime");
    std::puts("PASS: shared in-flight read, waiter timeout, leader completion");
    std::puts("PASS: short completion never publishes a partial block");
}
}  // namespace

int main() {
    try {
        CheckBudgets();
        Fixture fixture;
        CheckReads(fixture, false);
        CheckReads(fixture, true);
        CheckDrain(fixture);
        CheckBlocks(fixture);
        std::puts("PASS: synchronous storage baseline");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
