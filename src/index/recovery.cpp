#include "recovery.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace agent_io {
namespace {

constexpr std::size_t kHeaderBytes = 64;
constexpr std::uint64_t kMaxWalFileBytes = std::uint64_t{1} << 32;

Error Corrupt(std::string message) {
    return Error{ErrorCode::corruption, std::move(message)};
}

bool SameInstance(const InstanceId& left, const InstanceId& right) {
    return std::memcmp(left.data(), right.data(), left.size()) == 0;
}

bool AllZero(const InstanceId& value) {
    return std::all_of(value.begin(), value.end(),
                       [](std::byte byte) { return byte == std::byte{}; });
}

Result<std::vector<std::byte>> ReadWalFile(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return Error{ErrorCode::io_error, "Cannot stat WAL file: " + path.string()};
    if (size > kMaxWalFileBytes)
        return Corrupt("WAL file exceeds the supported recovery size: " + path.string());
    if (size < kHeaderBytes)
        return Corrupt("WAL file is shorter than its header: " + path.string());
    std::ifstream input(path, std::ios::binary);
    if (!input) return Error{ErrorCode::io_error, "Cannot open WAL file: " + path.string()};
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!input) return Error{ErrorCode::io_error, "Cannot read WAL file: " + path.string()};
    return bytes;
}

struct WalFile {
    WalFileHeader header{};
    std::vector<std::byte> bytes;
    std::string name;
};

}  // namespace

Result<RecoveryReport> ScanWalDirectory(const std::filesystem::path& wal_directory,
                                        const InstanceId& instance_id,
                                        std::uint32_t dimension,
                                        Sequence checkpoint_sequence,
                                        const WalOptions& options) {
    if (dimension == 0) return Error{ErrorCode::invalid_argument, "Recovery needs a dimension"};
    if (AllZero(instance_id))
        return Error{ErrorCode::invalid_argument, "Recovery needs an instance identity"};
    RecoveryReport report;

    std::error_code error;
    if (!std::filesystem::is_directory(wal_directory, error)) {
        // A fresh instance has no WAL yet; that is an empty history, not a fault.
        if (error && error != std::errc::no_such_file_or_directory)
            return Error{ErrorCode::io_error,
                         "Cannot inspect the WAL directory: " + wal_directory.string()};
        return report;
    }

    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(wal_directory, error)) {
        if (error) return Error{ErrorCode::io_error, "Cannot list the WAL directory"};
        if (!entry.is_regular_file(error) || error) continue;
        const auto name = entry.path().filename().string();
        if (name.size() < 5 || name.compare(name.size() - 4, 4, ".log") != 0) continue;
        paths.push_back(entry.path());
    }

    std::vector<WalFile> files;
    files.reserve(paths.size());
    for (const auto& path : paths) {
        auto bytes = ReadWalFile(path);
        if (const auto* failure = std::get_if<Error>(&bytes)) return *failure;
        WalFile file;
        file.bytes = std::get<std::vector<std::byte>>(std::move(bytes));
        auto header = DecodeWalFileHeader(
            std::span<const std::byte>(file.bytes.data(), kHeaderBytes));
        if (const auto* failure = std::get_if<Error>(&header)) return *failure;
        file.header = std::get<WalFileHeader>(header);
        file.name = path.filename().string();
        if (!SameInstance(file.header.instance_id, instance_id))
            return Corrupt("WAL file belongs to another instance: " + file.name);
        files.push_back(std::move(file));
    }

    // Ordering comes from the header, never from the file name: the name only
    // exists to tell two files apart.
    std::sort(files.begin(), files.end(), [](const WalFile& left, const WalFile& right) {
        if (left.header.start_sequence != right.header.start_sequence)
            return left.header.start_sequence < right.header.start_sequence;
        return left.name < right.name;
    });
    for (std::size_t index = 1; index < files.size(); ++index)
        if (files[index].header.start_sequence == files[index - 1].header.start_sequence)
            return Corrupt("Two WAL files claim the same start sequence: " + files[index].name);
    if (files.empty()) return report;

    Sequence expected = files.front().header.start_sequence;
    for (std::size_t file_index = 0; file_index < files.size(); ++file_index) {
        const auto& file = files[file_index];
        if (file.header.start_sequence != expected)
            return Corrupt("WAL files do not continue each other at sequence " +
                           std::to_string(file.header.start_sequence));
        const bool is_final = file_index + 1 == files.size();
        WalFileSummary summary;
        summary.name = file.name;
        summary.start_sequence = file.header.start_sequence;
        summary.file_bytes = file.bytes.size();

        const std::span<const std::byte> content(file.bytes);
        std::size_t offset = kHeaderBytes;
        while (offset < content.size()) {
            auto decoded = DecodeWalFrame(content.subspan(offset), dimension,
                                          options.max_frame_bytes);
            if (const auto* failure = std::get_if<Error>(&decoded)) return *failure;
            const auto& frame = std::get<DecodedWalFrame>(decoded);
            if (frame.status == WalDecodeStatus::need_more) {
                // Only the newest file may end mid-frame. Rotation happens
                // between synchronised groups, so a short tail in an older file
                // means acknowledged bytes were lost.
                if (!is_final)
                    return Corrupt("Non-final WAL file ends inside a frame: " + file.name);
                summary.discarded_tail_bytes = content.size() - offset;
                report.discarded_tail_bytes += summary.discarded_tail_bytes;
                break;
            }
            if (frame.sequence != expected)
                return Corrupt("WAL sequence is not contiguous at " +
                               std::to_string(frame.sequence));
            expected = frame.sequence + 1;
            summary.highest_sequence = frame.sequence;
            ++summary.frames;
            offset += static_cast<std::size_t>(frame.consumed_bytes);

            if (frame.sequence <= checkpoint_sequence) {
                ++report.ignored_checkpointed_records;
                continue;
            }
            ReplayedRecord record;
            record.operation = frame.operation;
            record.sequence = frame.sequence;
            record.id = frame.id;
            record.values.resize(dimension);
            std::memcpy(record.values.data(), frame.payload.data(),
                        static_cast<std::size_t>(dimension) * sizeof(float));
            report.records.push_back(std::move(record));
        }
        summary.bytes_read = offset;
        if (summary.highest_sequence > report.highest_sequence)
            report.highest_sequence = summary.highest_sequence;
        report.files.push_back(std::move(summary));
    }
    return report;
}

}  // namespace agent_io
