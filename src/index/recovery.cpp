#include "recovery.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>

namespace agent_io {
namespace {

constexpr auto kHeaderBytes = wal_detail::kWalHeaderBytes;

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

bool MatchesSequencePrefix(std::span<const std::byte> frame, Sequence expected) {
    constexpr std::size_t kSequenceOffset = 24;
    for (std::size_t i = 0; i < sizeof(expected) && kSequenceOffset + i < frame.size(); ++i) {
        if (frame[kSequenceOffset + i] != static_cast<std::byte>((expected >> (8 * i)) & 0xffu))
            return false;
    }
    return expected != 0;
}

struct WalFile {
    WalFileHeader header{};
    wal_detail::ScannedWalFile scanned;
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
    std::filesystem::directory_iterator entry(wal_directory, error), end;
    for (; !error && entry != end; entry.increment(error)) {
        if (entry->path().extension() == L".log") paths.push_back(entry->path());
    }
    if (error) return Error{ErrorCode::io_error, "Cannot list the WAL directory", error.value()};

    std::vector<WalFile> files;
    files.reserve(paths.size());
    for (const auto& path : paths) {
        auto scanned = wal_detail::ReadWalForRecovery(path);
        if (const auto* failure = std::get_if<Error>(&scanned)) return *failure;
        WalFile file;
        file.scanned = std::get<wal_detail::ScannedWalFile>(std::move(scanned));
        auto header = DecodeWalFileHeader(file.scanned.snapshot.encoded_header);
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
    if (expected > checkpoint_sequence && expected - checkpoint_sequence != 1)
        return Corrupt("WAL history has a gap after the checkpoint");
    for (std::size_t file_index = 0; file_index < files.size(); ++file_index) {
        auto& file = files[file_index];
        if (file.header.start_sequence != expected)
            return Corrupt("WAL files do not continue each other at sequence " +
                           std::to_string(file.header.start_sequence));
        const bool is_final = file_index + 1 == files.size();
        WalFileSummary summary;
        summary.name = file.name;
        summary.start_sequence = file.header.start_sequence;
        summary.file_bytes = file.scanned.bytes.size();

        const std::span<const std::byte> content(file.scanned.bytes);
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
                if (!MatchesSequencePrefix(content.subspan(offset), expected))
                    return Corrupt("Incomplete WAL frame does not match the next sequence");
                summary.discarded_tail_bytes = content.size() - offset;
                report.discarded_tail_bytes += summary.discarded_tail_bytes;
                break;
            }
            if (frame.sequence != expected)
                return Corrupt("WAL sequence is not contiguous at " +
                               std::to_string(frame.sequence));
            expected = frame.sequence == (std::numeric_limits<Sequence>::max)()
                           ? 0 : frame.sequence + 1;
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
        if (is_final) {
            const auto recovered = (std::max)(report.highest_sequence, checkpoint_sequence);
            if (offset == kHeaderBytes &&
                (file.header.start_sequence <= recovered ||
                 file.header.start_sequence - recovered != 1))
                return Corrupt("Empty latest WAL does not start at the next sequence");
            report.latest = LatestWalRecovery{std::move(file.scanned.snapshot), file.header, offset};
        }
        report.files.push_back(std::move(summary));
    }
    return report;
}

Result<std::uint64_t> RecycleWalFiles(const std::filesystem::path& wal_directory,
                                      const InstanceId& instance_id, Sequence up_to,
                                      std::uint64_t* failed) {
    if (failed != nullptr) *failed = 0;
    if (AllZero(instance_id))
        return Error{ErrorCode::invalid_argument, "Reclamation needs an instance identity"};
    std::error_code error;
    if (!std::filesystem::is_directory(wal_directory, error)) return std::uint64_t{0};

    std::uint64_t removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(wal_directory, error)) {
        if (error) return Error{ErrorCode::io_error, "Cannot list the WAL directory"};
        if (!entry.is_regular_file(error) || error) continue;
        const auto name = entry.path().filename().string();
        if (name.size() < 5 || name.compare(name.size() - 4, 4, ".log") != 0) continue;
        // The header is read and the stream closed before the file is touched
        // again: Windows refuses to delete a file whose reader still holds it,
        // and an open handle here would turn every reclamation into a no-op.
        WalFileHeader decoded{};
        bool usable = false;
        {
            std::ifstream input(entry.path(), std::ios::binary);
            if (input) {
                std::vector<std::byte> header_bytes(kHeaderBytes);
                input.read(reinterpret_cast<char*>(header_bytes.data()),
                           static_cast<std::streamsize>(header_bytes.size()));
                if (input.gcount() == static_cast<std::streamsize>(kHeaderBytes)) {
                    const auto header =
                        DecodeWalFileHeader(std::span<const std::byte>(header_bytes));
                    if (std::holds_alternative<WalFileHeader>(header)) {
                        decoded = std::get<WalFileHeader>(header);
                        usable = SameInstance(decoded.instance_id, instance_id);
                    }
                }
            }
        }
        if (!usable) {
            // A file that cannot be read or does not belong to this instance is
            // left alone; deleting it would be a guess.
            if (failed != nullptr) ++*failed;
            continue;
        }
        if (decoded.start_sequence > up_to) continue;
        std::error_code remove_error;
        if (std::filesystem::remove(entry.path(), remove_error))
            ++removed;
        else if (failed != nullptr)
            ++*failed;
    }
    return removed;
}

}  // namespace agent_io
