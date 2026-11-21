#include "agent_io/optimized_storage.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <future>
#include <list>
#include <mutex>
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

void CheckClock(Fixture& fixture) {
    auto budget = Budget();
    budget.reserve_bytes += budget.cache_bytes - 7600;
    budget.cache_bytes = 7600;
    auto governor = Take(CreateResourceGovernor(budget));
    SynchronousStorageOptions options{false, 4, 4};
    std::shared_ptr<MeteredReadBackend> backend =
        Take(CreateSynchronousReadBackend(governor, options));
    auto reader = Take(CreateOptimizedBlockReader(governor, backend, {4, 0, true}));
    auto registry = Take(CreateSynchronousFileRegistry(governor, options,
        [&](FileId file) { reader->Retire(file); }));
    auto file = Take(registry->Open(fixture.path));
    const auto read = [&](std::uint64_t offset) {
        const BlockLocation at{{file->Id(), offset, 4096}, file};
        return Take(reader->ReadBatch(std::span(&at, 1),
            {ReadPriority::demand, std::chrono::steady_clock::now() + 2s})).front();
    };
    auto pinned = read(0);
    Require(read(0)->Bytes()[0] == fixture.expected[0], "Cached content differs");
    Require(backend->Stats().io_reads_submitted == 1, "Hit sent a kernel read");
    auto fallback = read(4096);
    Require(reader->Stats().cache_resident_bytes == 4096, "Pinned block exceeded cache capacity");
    Require(fallback->Bytes()[0] == fixture.expected[4096], "Workspace fallback content differs");
    pinned.reset();
    auto replacement = read(4096);
    Require(backend->Stats().io_reads_submitted == 3, "Workspace fallback was cached");
    read(4096);
    Require(backend->Stats().io_reads_submitted == 3, "CLOCK failed to replace the released block");
    Take(registry->Retire(file->Id()));
    Require(reader->Stats().cache_resident_bytes == 0, "Retire kept a strong cache reference");
    read(4096);
    Require(reader->Stats().cache_resident_bytes == 0, "Retired file returned to cache");
    Take(reader->Drain());
    Require(replacement->Bytes()[0] == fixture.expected[4096], "Retire invalidated a held lease");
    std::puts("PASS: CLOCK budget, pinned fallback, eviction, retirement");
}

class ParkedBackend final : public MeteredReadBackend {
    struct Request {
        ReadRequest request;
        ReadCallback callback;
    };
    mutable std::mutex mutex_;
    std::condition_variable queued_;
    std::list<Request> requests_;
    ReadBackendStats stats_;
public:
    Result<Success> Submit(ReadRequest request, ReadCallback callback) override {
        std::lock_guard lock(mutex_);
        requests_.push_back(Request{std::move(request), std::move(callback)});
        ++stats_.io_reads_submitted;
        queued_.notify_all();
        return Success{};
    }
    bool WaitForDemand(std::size_t count) {
        std::unique_lock lock(mutex_);
        return queued_.wait_for(lock, 1s, [&] {
            return static_cast<std::size_t>(std::ranges::count_if(requests_, [](const auto& request) {
                return request.request.priority == ReadPriority::demand;
            })) >= count;
        });
    }
    void Finish(bool short_read, std::optional<ReadPriority> priority = std::nullopt) {
        std::optional<Request> request;
        {
            std::unique_lock lock(mutex_);
            auto found = requests_.end();
            Require(queued_.wait_for(lock, 2s, [&] {
                found = std::ranges::find_if(requests_, [&](const auto& entry) {
                    return !priority || entry.request.priority == *priority;
                });
                return found != requests_.end();
            }), "No queued read for test completion");
            request.emplace(std::move(*found));
            requests_.erase(found);
        }
        std::ranges::fill(request->request.buffer->WritableBytes(), std::byte{0x66});
        request->callback(ReadCompletion{request->request.request_id,
            request->request.block.byte_count - (short_read ? 1u : 0u)});
    }
    Result<Success> Drain() override { return Success{}; }
    ReadBackendStats Stats() const override {
        std::lock_guard lock(mutex_);
        return stats_;
    }
};

void CheckPrefetch(Fixture& fixture) {
    auto governor = Take(CreateResourceGovernor(Budget()));
    auto registry = Take(CreateSynchronousFileRegistry(governor, {false, 4, 4}));
    auto file = Take(registry->Open(fixture.path));
    auto backend = std::make_shared<ParkedBackend>();
    auto reader = Take(CreateOptimizedBlockReader(governor, backend, {4, 1, true}));
    const BlockLocation first{{file->Id(), 0, 4096}, file};
    const BlockLocation second{{file->Id(), 4096, 4096}, file};
    const auto deadline = [] { return std::chrono::steady_clock::now() + 2s; };
    auto initiator = std::make_shared<QueryReadState>();
    initiator->prefetch_limit = 1;
    {
        QueryReadScope scope(initiator);
        Require(Take(reader->TryPrefetch(first, deadline())) == PrefetchDisposition::admitted,
            "Prefetch not admitted");
        Require(Take(reader->TryPrefetch(first, deadline())) == PrefetchDisposition::already_present,
            "In-flight prefetch was not shared");
        Require(Take(reader->TryPrefetch(second, deadline())) == PrefetchDisposition::dropped,
            "Prefetch exceeded cap");
    }
    std::weak_ptr<QueryReadState> initiator_pin = initiator;
    initiator.reset();
    auto waiter = std::make_shared<QueryReadState>();
    std::weak_ptr<QueryReadState> waiter_pin = waiter;
    {
        QueryReadScope scope(std::move(waiter));
        Expect(reader->ReadBatch(std::span(&first, 1), {ReadPriority::demand,
            std::chrono::steady_clock::now() + 20ms}), ErrorCode::deadline_exceeded);
    }
    Require(!initiator_pin.expired() && !waiter_pin.expired(), "Timeout released query slot early");
    Require(backend->Stats().io_reads_submitted == 1, "Waiter submitted duplicate IO");
    Expect(reader->Quiesce(std::chrono::steady_clock::now()), ErrorCode::deadline_exceeded);
    backend->Finish(true);
    Require(initiator_pin.expired() && waiter_pin.expired(), "Completion leaked a query slot");
    Require(reader->Stats().cache_resident_bytes == 0, "Short prefetch was published");
    auto retry = std::async(std::launch::async, [&] {
        return reader->ReadBatch(std::span(&first, 1), {ReadPriority::demand, deadline()});
    });
    backend->Finish(true);
    Expect(retry.get(), ErrorCode::io_error);
    Require(Take(reader->TryPrefetch(first, deadline())) == PrefetchDisposition::admitted,
        "Prefetch could not retry after a short completion");
    backend->Finish(false);
    const auto use = Take(reader->ReadBatch(std::span(&first, 1),
        {ReadPriority::demand, deadline()}));
    Require(use.front()->Bytes()[0] == std::byte{0x66}, "Successful prefetch bytes differ");
    const auto stats = reader->Stats();
    Require(stats.prefetch_submitted == 2 && stats.prefetch_consumed == 1 &&
        stats.prefetch_ready_at_demand == 1 && stats.peak_prefetch_reads == 1,
        "Prefetch statistics exceed actual admissions or use");
    Take(reader->Drain());
    std::puts("PASS: prefetch quota, sharing, timeout ownership, short completion and retry");
}

class DeadlineBackend final : public SynchronousReadBackend {
public:
    std::chrono::steady_clock::time_point first_deadline;
    std::uint64_t submissions = 0;

    Result<Success> Submit(ReadRequest request, ReadCallback callback) override {
        if (++submissions == 1) std::this_thread::sleep_until(first_deadline + 10ms);
        std::ranges::fill(request.buffer->WritableBytes(), std::byte{0x66});
        callback(ReadCompletion{request.request_id, request.block.byte_count});
        return Success{};
    }
    Result<Success> Drain() override { return Success{}; }
    ReadBackendStats Stats() const override { return {}; }
};

void CheckBatchDispatch(Fixture& fixture) {
    using Clock = std::chrono::steady_clock;
    auto governor = Take(CreateResourceGovernor(Budget()));
    auto registry = Take(CreateSynchronousFileRegistry(governor, {false, 4, 3}));
    auto file = Take(registry->Open(fixture.path));
    const std::array demand{
        BlockLocation{{file->Id(), 4096, 4096}, file},
        BlockLocation{{file->Id(), 8192, 4096}, file},
        BlockLocation{{file->Id(), 12288, 4096}, file}};
    auto slow = std::make_shared<DeadlineBackend>();
    auto reader = Take(CreateOptimizedBlockReader(governor, slow, {3, 0, false}));
    slow->first_deadline = Clock::now() + 100ms;
    Expect(reader->ReadBatch(demand, {ReadPriority::demand, slow->first_deadline}),
        ErrorCode::deadline_exceeded);
    Require(slow->submissions == 1, "Submitted a new read after the batch deadline");
    Take(reader->Quiesce(Clock::now() + 1s));
    Take(reader->ReadBatch(demand, {ReadPriority::demand, Clock::now() + 1s}));
    Require(slow->submissions == 4, "Timed-out queue blocked a retry");
    Take(reader->Drain());

    auto parked = std::make_shared<ParkedBackend>();
    reader = Take(CreateOptimizedBlockReader(governor, parked, {3, 1, true}));
    const BlockLocation speculative{{file->Id(), 0, 4096}, file};
    Require(Take(reader->TryPrefetch(speculative, Clock::now() + 1s)) ==
        PrefetchDisposition::admitted, "Prefetch was not held in a slot");
    auto batch = std::async(std::launch::async, [&] {
        return reader->ReadBatch(demand, {ReadPriority::demand, Clock::now() + 3s});
    });
    Require(parked->WaitForDemand(2), "Demand did not use its two free slots");
    Require(parked->Stats().io_reads_submitted == 3, "Dispatched more than three reads");

    auto later = std::make_shared<QueryReadState>();
    std::weak_ptr<QueryReadState> queued_pin = later;
    {
        QueryReadScope scope(std::move(later));
        Expect(reader->ReadBatch(std::span(demand).last(1),
            {ReadPriority::demand, Clock::now() + 30ms}), ErrorCode::deadline_exceeded);
    }
    Require(queued_pin.expired(), "A queued read retained a timed-out waiter");
    parked->Finish(false, ReadPriority::demand);
    Require(parked->WaitForDemand(2), "Queued demand did not refill the freed slot");
    parked->Finish(false, ReadPriority::demand);
    parked->Finish(false, ReadPriority::demand);
    const auto blocks = Take(batch.get());
    for (std::size_t i = 0; i < demand.size(); ++i)
        Require(blocks[i]->Key() == demand[i].key, "Batched reads returned out of order");
    Require(parked->Stats().io_reads_submitted == 4, "Queued same-key read was dispatched twice");
    const auto stats = reader->Stats();
    Require(stats.peak_inflight_reads == 3 && stats.peak_prefetch_reads == 1 &&
        stats.cache_resident_bytes == 3 * 4096, "Incorrect slots or speculative completion");
    parked->Finish(false, ReadPriority::prefetch);
    Take(reader->Drain());
    std::puts("PASS: deadline stops new reads, queued sharing and demand use of free slots");
}

void CheckIocp(Fixture& fixture) {
    auto governor = Take(CreateResourceGovernor(Budget()));
    auto registry = Take(CreateSynchronousFileRegistry(governor, {true, 4, 2, true}));
    auto file = Take(registry->Open(fixture.path));
    auto backend = Take(CreateIocpReadBackend(governor, 2));
    auto first = Take(AllocateReadBuffer(governor, 4096, Take(ReadAlignment(*file))));
    auto second = Take(AllocateReadBuffer(governor, 4096, Take(ReadAlignment(*file))));
    std::promise<Result<ReadCompletion>> done_first, done_second;
    std::promise<void> entered, release;
    auto resume = release.get_future();
    Take(backend->Submit(Request(1, file, first, 0, 4096),
        [&](Result<ReadCompletion> result) {
            entered.set_value();
            resume.wait();
            done_first.set_value(std::move(result));
        }));
    entered.get_future().wait();
    const auto overlap = backend->Submit(Request(3, file, first, 4096, 4096),
        [](Result<ReadCompletion>) {});
    const auto submitted = backend->Submit(Request(2, file, second, 4096, 4096),
        [&](Result<ReadCompletion> result) { done_second.set_value(std::move(result)); });
    auto draining = std::async(std::launch::async, [&] { return backend->Drain(); });
    const bool waited = draining.wait_for(20ms) == std::future_status::timeout;
    release.set_value();
    Take(draining.get());
    Expect(overlap, ErrorCode::invalid_argument);
    Take(submitted);
    Require(waited, "Drain returned while IOCP callback held leases");
    Require(Take(done_first.get_future().get()).request_id == 1 &&
        Take(done_second.get_future().get()).request_id == 2, "IOCP completion IDs differ");
    Require(std::equal(first->Bytes().begin(), first->Bytes().end(), fixture.expected.begin()) &&
        std::equal(second->Bytes().begin(), second->Bytes().end(), fixture.expected.begin() + 4096),
        "IOCP returned bad bytes");

    // Use a fresh registry so the failure is submitted through its own port.
    registry = Take(CreateSynchronousFileRegistry(governor, {true, 4, 2, true}));
    file = Take(registry->Open(fixture.path));
    backend = Take(CreateIocpReadBackend(governor, 2));
    std::promise<Result<ReadCompletion>> failed;
    auto read_only = std::make_shared<ReadOnlyBuffer>();
    const auto invalid = backend->Submit(Request(1, file, read_only, 0, 4096),
        [&](Result<ReadCompletion> result) { failed.set_value(std::move(result)); });
    if (std::holds_alternative<Success>(invalid))
        Expect(failed.get_future().get(), ErrorCode::io_error);
    else
        Expect(invalid, ErrorCode::io_error);
    Take(backend->Drain());
    Require(backend->Stats().io_read_errors == 1, "IOCP failed read was not counted once");
    std::puts("PASS: native IOCP offsets, buffer exclusion, drain and read error");
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
        CheckClock(fixture);
        CheckPrefetch(fixture);
        CheckBatchDispatch(fixture);
        CheckIocp(fixture);
        std::puts("PASS: synchronous storage baseline");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
