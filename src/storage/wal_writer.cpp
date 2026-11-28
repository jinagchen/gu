#include "wal_native.hpp"

#include <objbase.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace agent_io::wal_detail {
namespace {

constexpr auto kHeaderBytes = kWalHeaderBytes;
constexpr std::uint64_t kMaxWalFileBytes = std::uint64_t{1} << 32;

struct Failure {
    ErrorCode code;
    const char* message;
    int system_error = 0;
    WriteEffect effect = WriteEffect::none;
    std::optional<Sequence> sequence = {};

    Error AsError() const { return {code, message, system_error, effect, sequence}; }
};

Failure NativeFailure(const char* message, DWORD system_error) {
    return {ErrorCode::io_error, message, static_cast<int>(system_error)};
}

class NativeFile {
public:
    NativeFile() = default;
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;
    ~NativeFile() { if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle); }

    std::optional<Failure> Close() {
        if (handle == INVALID_HANDLE_VALUE) return {};
        if (::CloseHandle(handle) == 0) return NativeFailure("Close WAL file", GetLastError());
        handle = INVALID_HANDLE_VALUE;
        return {};
    }

    HANDLE handle = INVALID_HANDLE_VALUE;
};

Result<WalFileSnapshot> InspectFile(HANDLE handle, const std::filesystem::path& path) {
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)))
        return NativeFailure("Inspect WAL file attributes", GetLastError()).AsError();
    if (attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
        return NativeFailure("WAL path must name a regular file", ERROR_INVALID_DATA).AsError();

    WalFileSnapshot snapshot;
    snapshot.path = path;
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &snapshot.identity, sizeof(snapshot.identity)))
        return NativeFailure("Read WAL file identity", GetLastError()).AsError();
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size))
        return NativeFailure("Read WAL file size", GetLastError()).AsError();
    snapshot.scanned_bytes = static_cast<std::uint64_t>(size.QuadPart);
    return snapshot;
}

std::optional<Failure> ReadExact(HANDLE handle, std::span<std::byte> output) {
    while (!output.empty()) {
        const auto requested = static_cast<DWORD>((std::min)(output.size(), std::size_t{8} << 20));
        DWORD read = 0;
        if (!ReadFile(handle, output.data(), requested, &read, nullptr))
            return NativeFailure("Read WAL file", GetLastError());
        if (read == 0) return NativeFailure("Unexpected EOF reading WAL file", ERROR_HANDLE_EOF);
        output = output.subspan(read);
    }
    return {};
}

Result<Success> VerifySnapshot(HANDLE file, const WalFileSnapshot& snapshot) {
    auto inspected = InspectFile(file, snapshot.path);
    if (const auto* error = std::get_if<Error>(&inspected)) return *error;
    const auto& current = std::get<WalFileSnapshot>(inspected);
    if (current.identity.VolumeSerialNumber != snapshot.identity.VolumeSerialNumber ||
        std::memcmp(&current.identity.FileId, &snapshot.identity.FileId, sizeof(FILE_ID_128)) != 0 ||
        current.scanned_bytes != snapshot.scanned_bytes)
        return NativeFailure("WAL identity or size changed during recovery",
                             ERROR_INVALID_DATA).AsError();
    std::array<std::byte, kHeaderBytes> encoded_header{};
    if (auto failure = ReadExact(file, encoded_header)) return failure->AsError();
    if (encoded_header != snapshot.encoded_header)
        return NativeFailure("WAL header changed during recovery", ERROR_INVALID_DATA).AsError();
    return Success{};
}

Result<Success> OpenVerified(NativeFile& file, const WalFileSnapshot& snapshot,
                             const NativeIo& io) {
    file.handle = io.open(snapshot.path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                         OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file.handle == INVALID_HANDLE_VALUE)
        return NativeFailure("Open existing WAL for writing", GetLastError()).AsError();
    return VerifySnapshot(file.handle, snapshot);
}

std::optional<Failure> ValidateWriter(const std::filesystem::path& directory,
                                     std::uint32_t dimension, const WalOptions& options,
                                     const NativeIo& io) {
    const auto size = WalFrameBytes(dimension);
    const auto* frame = std::get_if<std::uint64_t>(&size);
    if (directory.empty() || !frame || *frame > options.max_frame_bytes ||
        options.rotation_bytes < kHeaderBytes + static_cast<std::uint64_t>(options.max_frame_bytes) ||
        !io.write || !io.flush || !io.seek || !io.truncate || !io.publish || !io.open)
        return Failure{ErrorCode::invalid_argument, "Invalid WAL directory, dimension, or limits"};
    return {};
}

std::string GuidText(const GUID& guid) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(36);
    const auto field = [&](unsigned long value, unsigned digits) {
        for (unsigned i = digits; i > 0; --i)
            text.push_back(kHex[(value >> ((i - 1) * 4)) & 0xful]);
    };
    field(guid.Data1, 8);
    text += '-';
    field(guid.Data2, 4);
    text += '-';
    field(guid.Data3, 4);
    text += '-';
    for (std::size_t i = 0; i < 8; ++i) {
        if (i == 2) text += '-';
        field(guid.Data4[i], 2);
    }
    return text;
}

class Writer final : public WalWriter {
public:
    Writer(const std::filesystem::path& directory, const WalFileHeader& header,
           std::uint32_t dimension, const WalOptions& options, NativeIo io)
        : directory_(directory), instance_(header.instance_id), dimension_(dimension),
          options_(options), io_(io), next_(header.start_sequence) {}

    Result<Success> Start(const WalFileHeader& header) {
        std::array<std::byte, kHeaderBytes> encoded;
        auto result = EncodeWalFileHeader(header, encoded);
        if (auto* error = std::get_if<Error>(&result)) return std::move(*error);
        return Open(encoded, header.start_sequence, false);
    }

    void AdoptEmpty(NativeFile& file) {
        file_.handle = std::exchange(file.handle, INVALID_HANDLE_VALUE);
        file_bytes_ = kHeaderBytes;
    }

    Result<Success> Append(std::span<const std::byte> encoded_frame) override {
        if (const auto unavailable = Unavailable()) return unavailable->AsError();
        auto parsed = DecodeWalFrame(encoded_frame, dimension_, options_.max_frame_bytes);
        if (auto* error = std::get_if<Error>(&parsed)) {
            error->code = ErrorCode::invalid_argument;
            return std::move(*error);
        }
        const auto& frame = std::get<DecodedWalFrame>(parsed);
        if (frame.status != WalDecodeStatus::complete || frame.consumed_bytes != encoded_frame.size())
            return Failure{ErrorCode::invalid_argument, "Append requires exactly one complete WAL frame"}.AsError();
        if (next_ == 0 || frame.sequence != next_)
            return Failure{ErrorCode::invalid_argument, "WAL sequence is not the next sequence",
                           0, WriteEffect::none, frame.sequence}.AsError();
        if (frame.consumed_bytes > options_.rotation_bytes - file_bytes_)
            return Failure{ErrorCode::resource_exhausted, "WAL file rotation is required",
                           0, WriteEffect::none, frame.sequence}.AsError();

        if (auto failure = WriteAll(encoded_frame)) {
            failure->effect = WriteEffect::may_have_applied;
            failure->sequence = frame.sequence;
            return Stop(*failure);
        }
        file_bytes_ += frame.consumed_bytes;
        next_ = frame.sequence == std::numeric_limits<Sequence>::max() ? 0 : frame.sequence + 1;
        dirty_ = true;
        return Success{};
    }

    Result<Success> Sync() override {
        if (const auto unavailable = Unavailable()) return unavailable->AsError();
        if (auto failure = Flush()) {
            if (dirty_) failure->effect = WriteEffect::may_have_applied;
            return Stop(*failure);
        }
        dirty_ = false;
        return Success{};
    }

    Result<Success> Rotate(const WalFileHeader& next) override {
        if (const auto unavailable = Unavailable()) return unavailable->AsError();
        if (dirty_ || file_bytes_ == kHeaderBytes || next_ == 0 ||
            next.start_sequence != next_ || next.instance_id != instance_)
            return Failure{ErrorCode::invalid_argument,
                           "WAL rotation requires a synced group and a matching next header"}.AsError();
        std::array<std::byte, kHeaderBytes> encoded;
        auto result = EncodeWalFileHeader(next, encoded);
        if (auto* error = std::get_if<Error>(&result)) return std::move(*error);
        return Open(encoded, next.start_sequence, true);
    }

    Result<Success> Close() override {
        if (!closed_) {
            closed_ = true;
            if (auto close_error = file_.Close(); close_error && !failure_) {
                if (dirty_) close_error->effect = WriteEffect::may_have_applied;
                failure_ = close_error->AsError();
            }
        }
        if (failure_) return *failure_;
        return Success{};
    }

private:
    std::optional<Failure> Unavailable() const {
        if (closed_) return Failure{ErrorCode::closed, "WAL writer is closed"};
        if (failure_) return Failure{ErrorCode::recovery_required, "WAL writer stopped after a prior I/O failure",
                                    failure_->system_error};
        return {};
    }

    Error Stop(Failure failure) { return Stop(failure.AsError()); }

    Error Stop(Error error) {
        failure_ = error;
        return error;
    }

    std::optional<Failure> WriteAll(std::span<const std::byte> input) {
        // The header and each validated frame fit in a DWORD.
        while (!input.empty()) {
            const auto requested = static_cast<DWORD>(input.size());
            DWORD written = 0;
            if (io_.write(file_.handle, input.data(), requested, &written, nullptr) == 0)
                return NativeFailure("WriteFile for WAL", GetLastError());
            if (written == 0 || written > requested)
                return NativeFailure("WriteFile returned an invalid WAL write length", ERROR_WRITE_FAULT);
            input = input.subspan(written);
        }
        return {};
    }

    std::optional<Failure> Flush() {
        if (io_.flush(file_.handle) == 0)
            return NativeFailure("FlushFileBuffers for WAL", GetLastError());
        return {};
    }

    Result<Success> Open(std::span<const std::byte> encoded, Sequence first, bool rotating) {
        GUID id{};
        const HRESULT generated = CoCreateGuid(&id);
        if (FAILED(generated))
            return Failure{ErrorCode::io_error, "Generate WAL file UUID",
                           static_cast<int>(generated)}.AsError();
        // Build the path before releasing the old handle. Each file gets its own UUID.
        const auto path = directory_ / ("wal-" + std::to_string(first) + "-" + GuidText(id) + ".log");
        auto pending = path;
        pending.replace_extension(L".pending");
        if (rotating) {
            if (auto failure = Flush()) return Stop(*failure);
            if (auto failure = file_.Close()) return Stop(*failure);
        }
        file_.handle = io_.open(pending.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_.handle == INVALID_HANDLE_VALUE)
            return Stop(NativeFailure("Create pending WAL file", GetLastError()));
        if (auto failure = WriteAll(encoded)) return Stop(*failure);
        if (auto failure = Flush()) return Stop(*failure);
        auto inspected = InspectFile(file_.handle, path);
        if (auto* error = std::get_if<Error>(&inspected)) return Stop(std::move(*error));
        auto snapshot = std::get<WalFileSnapshot>(std::move(inspected));
        if (snapshot.scanned_bytes != encoded.size())
            return Stop(NativeFailure("Unexpected pending WAL size", ERROR_INVALID_DATA));
        std::copy(encoded.begin(), encoded.end(), snapshot.encoded_header.begin());
        if (auto failure = file_.Close()) return Stop(*failure);
        // No replace or copy flags: publication must be a same-volume rename.
        if (!io_.publish(pending.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH))
            return Stop(NativeFailure("Publish new WAL file header", GetLastError()));
        auto attached = OpenVerified(file_, snapshot, io_);
        if (auto* error = std::get_if<Error>(&attached)) return Stop(std::move(*error));
        file_bytes_ = encoded.size();
        return Success{};
    }

    NativeFile file_;
    const std::filesystem::path directory_;
    const std::array<std::byte, 16> instance_;
    const std::uint32_t dimension_;
    const WalOptions options_;
    const NativeIo io_;
    Sequence next_;
    std::uint64_t file_bytes_ = 0;
    bool dirty_ = false;
    bool closed_ = false;
    std::optional<Error> failure_;
};

}  // namespace

NativeIo& CurrentNativeIo() {
    static thread_local NativeIo io;
    return io;
}

Result<WalFileSnapshot> ReadWalHeaderForRecovery(const std::filesystem::path& path) {
    NativeFile file;
    file.handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file.handle == INVALID_HANDLE_VALUE)
        return NativeFailure("Open WAL for read-only recovery scan", GetLastError()).AsError();
    auto inspected = InspectFile(file.handle, path);
    if (const auto* error = std::get_if<Error>(&inspected)) return *error;
    auto snapshot = std::get<WalFileSnapshot>(std::move(inspected));
    const auto size = snapshot.scanned_bytes;
    if (size < kHeaderBytes || size > kMaxWalFileBytes)
        return Failure{ErrorCode::corruption, "WAL file size is outside the supported range"}.AsError();
    if (auto failure = ReadExact(file.handle, snapshot.encoded_header)) return failure->AsError();
    return snapshot;
}

Result<Success> ScanWalFrames(const WalFileSnapshot& snapshot, std::span<std::byte> buffer,
                             const std::function<Result<Success>(
                                 std::span<const std::byte>)>& visit) {
    if (buffer.empty())
        return Error{ErrorCode::invalid_argument, "WAL scan needs a frame buffer"};
    NativeFile file;
    file.handle = CreateFileW(snapshot.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file.handle == INVALID_HANDLE_VALUE)
        return NativeFailure("Open WAL frames for read-only recovery scan", GetLastError()).AsError();
    const auto verified = VerifySnapshot(file.handle, snapshot);
    if (const auto* error = std::get_if<Error>(&verified)) return *error;
    auto remaining = snapshot.scanned_bytes - kHeaderBytes;
    while (remaining != 0) {
        auto frame = buffer.first(static_cast<std::size_t>(
            (std::min)(remaining, static_cast<std::uint64_t>(buffer.size()))));
        if (auto failure = ReadExact(file.handle, frame)) return failure->AsError();
        const auto result = visit(frame);
        if (const auto* error = std::get_if<Error>(&result)) return *error;
        remaining -= frame.size();
    }
    return Success{};
}

Result<std::unique_ptr<WalWriter>> FinishWalRecovery(
    const WalFileSnapshot& snapshot, std::uint64_t valid_bytes,
    const WalFileHeader& header, std::uint32_t dimension, const WalOptions& options) {
    const auto io = CurrentNativeIo();
    if (valid_bytes < kHeaderBytes || valid_bytes > snapshot.scanned_bytes)
        return Failure{ErrorCode::invalid_argument, "Invalid WAL recovery offset"}.AsError();
    std::unique_ptr<Writer> writer;
    if (valid_bytes == kHeaderBytes) {
        std::error_code path_error;
        const auto directory = std::filesystem::absolute(snapshot.path.parent_path(), path_error);
        if (path_error)
            return Failure{ErrorCode::io_error, "Resolve WAL directory", path_error.value()}.AsError();
        if (auto failure = ValidateWriter(directory, dimension, options, io))
            return failure->AsError();
        writer = std::make_unique<Writer>(directory, header, dimension, options, io);
    }

    NativeFile file;
    auto verified = OpenVerified(file, snapshot, io);
    if (const auto* error = std::get_if<Error>(&verified)) return *error;

    if (valid_bytes < snapshot.scanned_bytes) {
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(valid_bytes);
        if (!io.seek(file.handle, position, nullptr, FILE_BEGIN))
            return NativeFailure("Position latest WAL for tail repair", GetLastError()).AsError();
        if (!io.truncate(file.handle))
            return NativeFailure("Truncate latest WAL tail", GetLastError()).AsError();
    }
    // A prior attempt may have changed the length without syncing it.
    if (!io.flush(file.handle))
        return NativeFailure("Flush latest WAL during recovery", GetLastError()).AsError();
    if (writer) {
        writer->AdoptEmpty(file);
    } else if (auto failure = file.Close()) {
        return failure->AsError();
    }
    return std::unique_ptr<WalWriter>{std::move(writer)};
}

Result<std::unique_ptr<WalWriter>> CreateWriter(
    const std::filesystem::path& directory, const WalFileHeader& header,
    std::uint32_t dimension, const WalOptions& options, NativeIo io) {
    if (auto failure = ValidateWriter(directory, dimension, options, io))
        return failure->AsError();
    std::error_code path_error;
    const auto absolute_directory = std::filesystem::absolute(directory, path_error);
    if (path_error) return Failure{ErrorCode::io_error, "Resolve WAL directory", path_error.value()}.AsError();
    auto writer = std::make_unique<Writer>(absolute_directory, header, dimension, options, io);
    auto result = writer->Start(header);
    if (auto* error = std::get_if<Error>(&result)) return std::move(*error);
    return std::unique_ptr<WalWriter>{std::move(writer)};
}

}  // namespace agent_io::wal_detail

namespace agent_io {

Result<std::unique_ptr<WalWriter>> CreateWalWriter(
    const std::filesystem::path& directory, const WalFileHeader& header,
    std::uint32_t dimension, const WalOptions& options) {
    return wal_detail::CreateWriter(directory, header, dimension, options,
                                    wal_detail::CurrentNativeIo());
}

}  // namespace agent_io
