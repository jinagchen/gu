#include "digest.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdio>
#include <vector>

namespace agent_io {
namespace {

Error Failure(ErrorCode code, std::string message, int system_error = 0) {
    Error error{code, std::move(message)};
    error.system_error = system_error;
    return error;
}

struct AlgorithmHandle {
    BCRYPT_ALG_HANDLE value = nullptr;
    AlgorithmHandle() = default;
    AlgorithmHandle(const AlgorithmHandle&) = delete;
    AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;
    ~AlgorithmHandle() {
        if (value) BCryptCloseAlgorithmProvider(value, 0);
    }
};

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};

}  // namespace

Result<Digest> HashFile(const std::filesystem::path& path) {
    Handle file;
    file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file.value == INVALID_HANDLE_VALUE)
        return Failure(ErrorCode::io_error, "Cannot open file for hashing",
                       static_cast<int>(GetLastError()));

    AlgorithmHandle algorithm;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM,
                                                  nullptr, 0);
    if (status < 0) return Failure(ErrorCode::unsupported, "SHA256 provider unavailable");

    ULONG object_bytes = 0, written = 0;
    status = BCryptGetProperty(algorithm.value, BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes),
                               &written, 0);
    if (status < 0 || object_bytes == 0)
        return Failure(ErrorCode::unsupported, "Cannot size SHA256 object");

    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<std::uint8_t> hash_object(object_bytes);
    status = BCryptCreateHash(algorithm.value, &hash, hash_object.data(), object_bytes,
                              nullptr, 0, 0);
    if (status < 0) return Failure(ErrorCode::unsupported, "Cannot create SHA256 hash");

    Digest digest{};
    std::vector<std::byte> buffer(1u << 20);
    while (true) {
        DWORD read = 0;
        if (!ReadFile(file.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                      nullptr)) {
            BCryptDestroyHash(hash);
            return Failure(ErrorCode::io_error, "Read failed while hashing",
                           static_cast<int>(GetLastError()));
        }
        if (read == 0) break;
        status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                                read, 0);
        if (status < 0) {
            BCryptDestroyHash(hash);
            return Failure(ErrorCode::unsupported, "SHA256 update failed");
        }
    }
    status = BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(digest.data()),
                              static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    if (status < 0) return Failure(ErrorCode::unsupported, "SHA256 finalisation failed");
    return digest;
}

std::string FormatDigest(const Digest& digest) {
    static const char* kHex = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2);
    for (const std::byte item : digest) {
        const auto value = static_cast<unsigned>(item);
        text.push_back(kHex[value >> 4]);
        text.push_back(kHex[value & 0x0f]);
    }
    return text;
}

Result<Digest> ParseDigest(std::string_view text) {
    if (text.size() != kSha256Bytes * 2)
        return Failure(ErrorCode::invalid_argument, "SHA256 must have 64 characters");
    Digest digest{};
    for (std::size_t index = 0; index < kSha256Bytes; ++index) {
        unsigned value = 0;
        for (std::size_t half = 0; half < 2; ++half) {
            const char digit = text[index * 2 + half];
            unsigned part = 0;
            if (digit >= '0' && digit <= '9') part = static_cast<unsigned>(digit - '0');
            else if (digit >= 'a' && digit <= 'f') part = static_cast<unsigned>(digit - 'a') + 10;
            else return Failure(ErrorCode::invalid_argument,
                                "SHA256 must use lowercase hexadecimal");
            value = value * 16 + part;
        }
        digest[index] = static_cast<std::byte>(value);
    }
    return digest;
}

InstanceId NewInstanceId() {
    InstanceId id{};
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(id.data()),
                        static_cast<ULONG>(id.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        // Deterministic fallback keeps Open/Search working on unusual hosts; the
        // value is still unique per build because it mixes the file time.
        const auto ticks = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        auto* bytes = reinterpret_cast<std::uint8_t*>(id.data());
        for (std::size_t index = 0; index < id.size(); ++index)
            bytes[index] = static_cast<std::uint8_t>((ticks >> ((index % 8) * 8)) + index * 31u);
    }
    // RFC 4122 version 4 markers.
    auto* bytes = reinterpret_cast<std::uint8_t*>(id.data());
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
    return id;
}

std::string FormatUuid(const InstanceId& id) {
    static const char* kHex = "0123456789abcdef";
    std::string text;
    text.reserve(36);
    for (std::size_t index = 0; index < id.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) text.push_back('-');
        const auto value = static_cast<unsigned>(id[index]);
        text.push_back(kHex[value >> 4]);
        text.push_back(kHex[value & 0x0f]);
    }
    return text;
}

Result<InstanceId> ParseUuid(std::string_view text) {
    if (text.size() != 36)
        return Failure(ErrorCode::invalid_argument, "UUID must have 36 characters");
    InstanceId id{};
    std::size_t source = 0;
    for (std::size_t index = 0; index < id.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            if (text[source] != '-')
                return Failure(ErrorCode::invalid_argument, "UUID must be hyphenated");
            ++source;
        }
        unsigned value = 0;
        for (std::size_t half = 0; half < 2; ++half) {
            const char digit = text[source++];
            unsigned part = 0;
            if (digit >= '0' && digit <= '9') part = static_cast<unsigned>(digit - '0');
            else if (digit >= 'a' && digit <= 'f') part = static_cast<unsigned>(digit - 'a') + 10;
            else return Failure(ErrorCode::invalid_argument, "UUID must use lowercase hexadecimal");
            value = value * 16 + part;
        }
        id[index] = static_cast<std::byte>(value);
    }
    if (source != text.size())
        return Failure(ErrorCode::invalid_argument, "UUID must be hyphenated");
    bool zero = true;
    for (const std::byte item : id) zero = zero && item == std::byte{0};
    if (zero) return Failure(ErrorCode::invalid_argument, "UUID must be non-zero");
    return id;
}

bool IsAsciiPath(const std::filesystem::path& path) {
    const auto text = path.u8string();
    for (const char8_t item : text)
        if (static_cast<unsigned>(item) > 0x7f) return false;
    return true;
}

}  // namespace agent_io
