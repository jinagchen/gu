#include "wal_native.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <string_view>

namespace {
using namespace agent_io;
using Bytes = std::span<const std::byte>;

constexpr auto kUuid = std::array{
    std::byte{0x00}, std::byte{0x11}, std::byte{0x22}, std::byte{0x33},
    std::byte{0x44}, std::byte{0x55}, std::byte{0x46}, std::byte{0x77},
    std::byte{0x88}, std::byte{0x99}, std::byte{0xaa}, std::byte{0xbb},
    std::byte{0xcc}, std::byte{0xdd}, std::byte{0xee}, std::byte{0xff}};
constexpr Sequence kGoldenSequence = 0x0102030405060708;
constexpr std::array<float, 2> kValues{1.5f, -2.25f};
constexpr DWORD kChildExit = 42;

void Require(bool condition, const char* message,
             std::source_location location = std::source_location::current()) {
    if (!condition)
        throw std::runtime_error("line " + std::to_string(location.line()) + ": " + message);
}

template<class T> T Take(Result<T> value) {
    if (const auto* error = std::get_if<Error>(&value))
        throw std::runtime_error(error->message + " (" + std::to_string(error->system_error) + ")");
    return std::get<T>(std::move(value));
}

template<class T> Error Expect(Result<T> value, ErrorCode code,
    WriteEffect effect = WriteEffect::none,
    std::source_location location = std::source_location::current()) {
    const auto* error = std::get_if<Error>(&value);
    Require(error && error->code == code && error->write_effect == effect,
            (error ? error->message.c_str() : "unexpected success"), location);
    return *error;
}

unsigned HexDigit(char c) {
    return static_cast<unsigned>(c >= 'a' ? c - 'a' + 10 : c - '0');
}

std::vector<std::byte> Hex(std::string_view text) {
    std::vector<std::byte> bytes;
    for (std::size_t i = 0; i < text.size(); i += 2)
        bytes.push_back(static_cast<std::byte>(16 * HexDigit(text[i]) + HexDigit(text[i + 1])));
    return bytes;
}

std::array<std::byte, 80> Frame(Sequence seq) {
    std::array<std::byte, 80> frame{};
    Require(Take(EncodeWalFrame({WalOperation::upsert, seq, 0x1122334455667788, kValues},
                                frame)) == frame.size(), "frame size");
    return frame;
}

void CheckCodec() {
    const auto golden_header = Hex(
        "4147494f4c4f4731010000004000000000112233445546778899aabbccddeeff"
        "0807060504030201000000000000000000000000000000000000000077ac6818");
    const auto golden_frame = Hex(
        "4147494f57414c31010002004000000050000000000000000807060504030201"
        "8877665544332211020000000000000008000000000000000000000000000000"
        "0000c03f000010c063e637ed00000000");
    std::array<std::byte, 64> header{};
    Take(EncodeWalFileHeader({kUuid, kGoldenSequence}, header));
    Require(std::ranges::equal(header, golden_header), "golden file header");
    const auto decoded = Take(DecodeWalFileHeader(golden_header));
    Require(decoded.instance_id == kUuid && decoded.start_sequence == kGoldenSequence, "header fields");
    for (std::size_t offset : {0u, 8u, 12u, 16u, 32u, 59u, 60u}) {
        auto bad = header;
        bad[offset] ^= std::byte{1};
        Expect(DecodeWalFileHeader(bad), ErrorCode::corruption);
    }
    Expect(DecodeWalFileHeader(Bytes(header).first(63)), ErrorCode::corruption);
    Expect(EncodeWalFileHeader({{}, 1}, header), ErrorCode::invalid_argument);
    Expect(EncodeWalFileHeader({kUuid, 0}, header), ErrorCode::invalid_argument);
    Require(std::ranges::equal(header, golden_header), "invalid header encode touched buffer");

    const auto frame = Frame(kGoldenSequence);
    Require(std::ranges::equal(frame, golden_frame), "golden frame");
    auto concatenated = golden_frame;
    concatenated.insert(concatenated.end(), frame.begin(), frame.end());
    const auto record = Take(DecodeWalFrame(concatenated, 2, 80));
    Require(record.status == WalDecodeStatus::complete && record.operation == WalOperation::upsert &&
            record.id == 0x1122334455667788 && record.sequence == kGoldenSequence &&
            record.dimension == 2 && record.consumed_bytes == 80 &&
            record.payload.data() == concatenated.data() + 64 && record.payload.size() == 8,
            "decoded fields and zero-copy payload");
    for (std::size_t size = 0; size < frame.size(); ++size) {
        const auto partial = Take(DecodeWalFrame(Bytes(frame).first(size), 2, 80));
        Require(partial.status == WalDecodeStatus::need_more && partial.consumed_bytes == 0 &&
                partial.payload.empty() && partial.sequence == 0 && partial.dimension == 0,
                "valid truncated prefix");
    }
    for (std::size_t offset : {0u, 8u, 10u, 12u, 16u, 40u, 44u, 48u, 56u}) {
        auto bad = frame;
        bad[offset] ^= std::byte{0x80};
        Expect(DecodeWalFrame(Bytes(bad).first(offset + 1), 2, 80), ErrorCode::corruption);
    }
    for (std::size_t offset : {64u, 72u, 76u}) {
        auto bad = frame;
        bad[offset] ^= std::byte{1};
        Expect(DecodeWalFrame(bad, 2, 80), ErrorCode::corruption);
    }
    auto bad = frame;
    std::fill_n(bad.begin() + 24, 8, std::byte{});
    Expect(DecodeWalFrame(Bytes(bad).first(32), 2, 80), ErrorCode::corruption);
    Expect(DecodeWalFrame(frame, 0, 80), ErrorCode::invalid_argument);
    Expect(DecodeWalFrame(frame, 2, 79), ErrorCode::invalid_argument);
    Expect(WalFrameBytes(0), ErrorCode::invalid_argument);
    Require(Take(WalFrameBytes(UINT32_MAX)) == 17179869248ull, "largest dimension arithmetic");
    auto output = frame;
    Expect(EncodeWalFrame({WalOperation::insert, 1, 0, kValues}, std::span(output).first(79)),
           ErrorCode::resource_exhausted);
    std::array<float, 2> nonfinite{0.0f, std::numeric_limits<float>::infinity()};
    Expect(EncodeWalFrame({WalOperation::upsert, 1, 0, nonfinite}, output), ErrorCode::invalid_argument);
    Require(output == frame, "invalid encode touched output");
}

struct Fixture {
    std::filesystem::path path;
    Fixture() {
        wchar_t directory[MAX_PATH]{}, name[MAX_PATH]{};
        Require(GetTempPathW(MAX_PATH, directory) != 0, "temp directory");
        Require(GetTempFileNameW(directory, L"wal", 0, name) != 0, "temp name");
        Require(DeleteFileW(name) != 0 && CreateDirectoryW(name, nullptr) != 0, "create temp directory");
        path = name;
    }
    ~Fixture() {
        std::error_code ignored;
        std::filesystem::directory_iterator file(path, ignored), end;
        while (!ignored && file != end) {
            std::error_code cleanup;
            std::filesystem::remove(file->path(), cleanup);
            file.increment(ignored);
        }
        std::filesystem::remove(path, ignored);
    }
};

std::vector<std::byte> ReadLog(const std::filesystem::path& directory, Sequence first,
                               std::size_t expected_size) {
    const auto prefix = L"wal-" + std::to_wstring(first) + L"-";
    std::filesystem::path path;
    for (const auto& item : std::filesystem::directory_iterator(directory)) {
        const auto name = item.path().filename().wstring();
        if (!name.starts_with(prefix)) continue;
        Require(path.empty() && name.size() == prefix.size() + 36 + 4 &&
                item.path().extension() == L".log", "WAL filename");
        path = item.path();
    }
    Require(!path.empty() && std::filesystem::file_size(path) == expected_size, "WAL file size");
    std::ifstream file(path, std::ios::binary);
    std::vector<std::byte> bytes(expected_size);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(static_cast<bool>(file), "read WAL file");
    return bytes;
}

void CheckWriter() {
    Fixture fixture;
    constexpr WalOptions limit{224, 80};
    Expect(CreateWalWriter(fixture.path, {kUuid, 1}, 2, {143, 80}), ErrorCode::invalid_argument);
    Expect(CreateWalWriter(fixture.path / L"missing", {kUuid, 1}, 2, limit), ErrorCode::io_error);
    auto writer = Take(CreateWalWriter(fixture.path, {kUuid, 1}, 2, limit));
    Expect(writer->Rotate({kUuid, 1}), ErrorCode::invalid_argument);
    Expect(writer->Append(Frame(2)), ErrorCode::invalid_argument);
    Expect(writer->Append(Bytes(Frame(1)).first(76)), ErrorCode::invalid_argument);
    Take(writer->Append(Frame(1)));
    Expect(writer->Rotate({kUuid, 2}), ErrorCode::invalid_argument);
    Take(writer->Append(Frame(2)));
    Require(Expect(writer->Append(Frame(3)), ErrorCode::resource_exhausted).attempted_sequence == 3,
            "rotation limit sequence");
    Take(writer->Sync());
    auto wrong = kUuid;
    wrong[5] ^= std::byte{1};
    Expect(writer->Rotate({wrong, 3}), ErrorCode::invalid_argument);
    Expect(writer->Rotate({kUuid, 4}), ErrorCode::invalid_argument);
    Take(writer->Rotate({kUuid, 3}));
    Expect(writer->Append(Frame(2)), ErrorCode::invalid_argument);
    Take(writer->Append(Frame(3)));
    Take(writer->Sync());
    Take(writer->Close());
    Take(writer->Close());
    Expect(writer->Append(Frame(4)), ErrorCode::closed);

    const auto old = ReadLog(fixture.path, 1, 224);
    Require(Take(DecodeWalFileHeader(Bytes(old).first(64))).start_sequence == 1 &&
            std::ranges::equal(Bytes(old).subspan(64, 80), Frame(1)) &&
            std::ranges::equal(Bytes(old).subspan(144), Frame(2)), "old WAL unchanged");
    const auto next = ReadLog(fixture.path, 3, 144);
    const auto header = Take(DecodeWalFileHeader(Bytes(next).first(64)));
    Require(header.instance_id == kUuid && header.start_sequence == 3 &&
            std::ranges::equal(Bytes(next).subspan(64), Frame(3)), "new WAL content");

    constexpr auto last = std::numeric_limits<Sequence>::max();
    writer = Take(CreateWalWriter(fixture.path, {kUuid, last}, 2, limit));
    Take(writer->Append(Frame(last)));
    Take(writer->Sync());
    Expect(writer->Append(Frame(1)), ErrorCode::invalid_argument);
    Expect(writer->Rotate({kUuid, 0}), ErrorCode::invalid_argument);
    Take(writer->Close());
}

enum class Fault { none, short_write, partial_then_fail, zero_write, kill_after_partial };
Fault fault{};
unsigned writes = 0, flushes = 0, fail_flush_at = 0;

BOOL WINAPI TestWrite(HANDLE handle, LPCVOID data, DWORD size, LPDWORD written, LPOVERLAPPED overlap) {
    ++writes;
    if (fault == Fault::partial_then_fail && writes == 2) {
        SetLastError(ERROR_DISK_FULL);
        return FALSE;
    }
    if (fault == Fault::zero_write) {
        *written = 0;
        return TRUE;
    }
    if (fault != Fault::none && writes == 1) size /= 2;
    const BOOL ok = ::WriteFile(handle, data, size, written, overlap);
    if (fault == Fault::kill_after_partial && ok && writes == 1 &&
        ::FlushFileBuffers(handle) != 0)
        TerminateProcess(GetCurrentProcess(), kChildExit);
    return ok;
}

BOOL WINAPI TestFlush(HANDLE handle) {
    if (++flushes == fail_flush_at) {
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    return ::FlushFileBuffers(handle);
}

std::unique_ptr<WalWriter> TestWriter(const Fixture& fixture, Sequence first) {
    fault = Fault::none;
    writes = flushes = fail_flush_at = 0;
    auto writer = Take(wal_detail::CreateWriter(fixture.path, {kUuid, first}, 2, {}, {TestWrite, TestFlush}));
    Require(writes == 1 && flushes == 1, "header must be written and flushed");
    writes = 0;
    return writer;
}

void CheckFaults() {
    Fixture fixture;
    auto writer = TestWriter(fixture, 10);
    fault = Fault::short_write;
    Take(writer->Append(Frame(10)));
    Require(writes == 2 && flushes == 1, "append must retry short progress without flushing");
    Take(writer->Close());
    Require(flushes == 1, "close flushed without explicit sync");
    const auto full = ReadLog(fixture.path, 10, 144);
    Require(std::ranges::equal(Bytes(full).subspan(64), Frame(10)), "short write completion");

    writer = TestWriter(fixture, 20);
    fault = Fault::partial_then_fail;
    const auto partial = Expect(writer->Append(Frame(20)), ErrorCode::io_error, WriteEffect::may_have_applied);
    Require(partial.system_error == ERROR_DISK_FULL && partial.attempted_sequence == 20, "short write error");
    Expect(writer->Append(Frame(21)), ErrorCode::recovery_required);
    Expect(writer->Sync(), ErrorCode::recovery_required);
    Expect(writer->Rotate({kUuid, 21}), ErrorCode::recovery_required);
    const auto prior = Expect(writer->Close(), ErrorCode::io_error, WriteEffect::may_have_applied);
    Require(prior.system_error == ERROR_DISK_FULL && writes == 2 && flushes == 1, "no I/O after failure");
    Expect(writer->Close(), ErrorCode::io_error, WriteEffect::may_have_applied);
    const auto short_file = ReadLog(fixture.path, 20, 104);
    Require(Take(DecodeWalFrame(Bytes(short_file).subspan(64), 2, 80)).status ==
                WalDecodeStatus::need_more, "partial WAL frame on disk");

    writer = TestWriter(fixture, 30);
    fault = Fault::zero_write;
    Expect(writer->Append(Frame(30)), ErrorCode::io_error, WriteEffect::may_have_applied);
    Require(writes == 1, "zero progress looped");
    Expect(writer->Close(), ErrorCode::io_error, WriteEffect::may_have_applied);

    writer = TestWriter(fixture, 40);
    Take(writer->Append(Frame(40)));
    fail_flush_at = 2;
    const auto sync = Expect(writer->Sync(), ErrorCode::io_error, WriteEffect::may_have_applied);
    Require(sync.system_error == ERROR_WRITE_FAULT, "flush error");
    Expect(writer->Append(Frame(41)), ErrorCode::recovery_required);
    Expect(writer->Close(), ErrorCode::io_error, WriteEffect::may_have_applied);

    writer = TestWriter(fixture, 50);
    Take(writer->Append(Frame(50)));
    Take(writer->Sync());
    fail_flush_at = 4;  // Old file flushes; the new header flush fails.
    Expect(writer->Rotate({kUuid, 51}), ErrorCode::io_error);
    Expect(writer->Append(Frame(51)), ErrorCode::recovery_required);
    Expect(writer->Close(), ErrorCode::io_error);
    ReadLog(fixture.path, 50, 144);
    ReadLog(fixture.path, 51, 64);

    fail_flush_at = flushes + 1;
    Expect(wal_detail::CreateWriter(fixture.path, {kUuid, 60}, 2, {}, {TestWrite, TestFlush}),
           ErrorCode::io_error);
    ReadLog(fixture.path, 60, 64);
}

void CrashChild(const std::filesystem::path& directory) {
    auto writer = Take(wal_detail::CreateWriter(directory, {kUuid, 1}, 2, {}, {TestWrite, TestFlush}));
    Take(writer->Append(Frame(1)));
    Take(writer->Sync());
    writes = 0;
    fault = Fault::kill_after_partial;
    Take(writer->Append(Frame(2)));
    throw std::runtime_error("test child did not terminate during the partial append");
}

void CheckProcessExit() {
    Fixture fixture;
    wchar_t path[MAX_PATH]{};
    const DWORD size = GetModuleFileNameW(nullptr, path, MAX_PATH);
    Require(size != 0 && size < MAX_PATH, "test executable path");
    auto command = L"\"" + std::wstring(path) + L"\" --crash \"" + fixture.path.native() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    Require(CreateProcessW(path, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &startup, &child) != 0, "create crash test process");
    CloseHandle(child.hThread);
    const DWORD wait = WaitForSingleObject(child.hProcess, 5000);
    DWORD code = 0;
    const BOOL got_exit = wait == WAIT_OBJECT_0 && GetExitCodeProcess(child.hProcess, &code);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(child.hProcess, 1);
        WaitForSingleObject(child.hProcess, 5000);
    }
    CloseHandle(child.hProcess);
    Require(got_exit && code == kChildExit, "child must exit by forced termination");
    const auto log = ReadLog(fixture.path, 1, 184);
    Require(Take(DecodeWalFileHeader(Bytes(log).first(64))).start_sequence == 1, "crash header");
    const auto durable = Take(DecodeWalFrame(Bytes(log).subspan(64), 2, 80));
    Require(durable.status == WalDecodeStatus::complete && durable.sequence == 1 &&
            durable.consumed_bytes == 80, "synced frame after process exit");
    Require(Take(DecodeWalFrame(Bytes(log).subspan(144), 2, 80)).status ==
                WalDecodeStatus::need_more, "partial frame after process exit");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring_view(argv[1]) == L"--crash") CrashChild(argv[2]);
        CheckCodec();
        CheckWriter();
        CheckFaults();
        CheckProcessExit();
        std::puts("WAL codec, writer, native fault injection, and process-exit checks passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
