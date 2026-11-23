#pragma once

#include "agent_io/storage.hpp"

#include <windows.h>

namespace agent_io::wal_detail {

// File creation and ownership stay native; tests can drive short writes and flush failures.
struct NativeIo {
    decltype(&::WriteFile) write = ::WriteFile;
    decltype(&::FlushFileBuffers) flush = ::FlushFileBuffers;
};

Result<std::unique_ptr<WalWriter>> CreateWriter(
    const std::filesystem::path& directory, const WalFileHeader& header,
    std::uint32_t dimension, const WalOptions& options, NativeIo io);

}  // namespace agent_io::wal_detail
