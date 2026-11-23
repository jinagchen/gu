#include "wal_native.hpp"

#include <objbase.h>

#include <limits>
#include <utility>

namespace agent_io::wal_detail {
namespace {

constexpr std::size_t kHeaderBytes = 64;

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
                failure_ = *close_error;
            }
        }
        if (failure_) return failure_->AsError();
        return Success{};
    }

private:
    std::optional<Failure> Unavailable() const {
        if (closed_) return Failure{ErrorCode::closed, "WAL writer is closed"};
        if (failure_) return Failure{ErrorCode::recovery_required, "WAL writer stopped after a prior I/O failure",
                                    failure_->system_error};
        return {};
    }

    Error Stop(Failure failure) {
        failure_ = failure;
        return failure.AsError();
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
        if (rotating) {
            if (auto failure = Flush()) return Stop(*failure);
            if (auto failure = file_.Close()) return Stop(*failure);
        }
        file_.handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_.handle == INVALID_HANDLE_VALUE)
            return Stop(NativeFailure("Create WAL file", GetLastError()));
        if (auto failure = WriteAll(encoded)) return Stop(*failure);
        if (auto failure = Flush()) return Stop(*failure);
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
    std::optional<Failure> failure_;
};

}  // namespace

Result<std::unique_ptr<WalWriter>> CreateWriter(
    const std::filesystem::path& directory, const WalFileHeader& header,
    std::uint32_t dimension, const WalOptions& options, NativeIo io) {
    const auto size = WalFrameBytes(dimension);
    const auto* frame = std::get_if<std::uint64_t>(&size);
    if (directory.empty() || !frame || *frame > options.max_frame_bytes ||
        options.rotation_bytes < kHeaderBytes + static_cast<std::uint64_t>(options.max_frame_bytes) ||
        !io.write || !io.flush)
        return Failure{ErrorCode::invalid_argument, "Invalid WAL directory, dimension, or limits"}.AsError();
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
    return wal_detail::CreateWriter(directory, header, dimension, options, {});
}

}  // namespace agent_io
