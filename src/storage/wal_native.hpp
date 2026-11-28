#pragma once

#include "agent_io/storage.hpp"

#include <functional>
#include <windows.h>

namespace agent_io::wal_detail {

inline constexpr std::size_t kWalHeaderBytes = 64;

struct NativeIo {
    decltype(&::WriteFile) write = ::WriteFile;
    decltype(&::FlushFileBuffers) flush = ::FlushFileBuffers;
    decltype(&::SetFilePointerEx) seek = ::SetFilePointerEx;
    decltype(&::SetEndOfFile) truncate = ::SetEndOfFile;
    decltype(&::MoveFileExW) publish = ::MoveFileExW;
    decltype(&::CreateFileW) open = ::CreateFileW;
};

// Thread-local so engine tests can inject faults through the normal entry points.
NativeIo& CurrentNativeIo();

struct WalFileSnapshot {
    std::filesystem::path path;
    FILE_ID_INFO identity{};
    std::uint64_t scanned_bytes = 0;
    std::array<std::byte, kWalHeaderBytes> encoded_header{};
};

Result<WalFileSnapshot> ReadWalHeaderForRecovery(const std::filesystem::path& path);
Result<Success> ScanWalFrames(const WalFileSnapshot& snapshot, std::span<std::byte> buffer,
                             const std::function<Result<Success>(
                                 std::span<const std::byte>)>& visit);

// A successful result holds a writer only when the latest log is empty after
// tail repair. The writer takes ownership of the handle used for the flush.
Result<std::unique_ptr<WalWriter>> FinishWalRecovery(
    const WalFileSnapshot& snapshot, std::uint64_t valid_bytes,
    const WalFileHeader& header, std::uint32_t dimension, const WalOptions& options);

Result<std::unique_ptr<WalWriter>> CreateWriter(
    const std::filesystem::path& directory, const WalFileHeader& header,
    std::uint32_t dimension, const WalOptions& options, NativeIo io);

}  // namespace agent_io::wal_detail
