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
    wal_detail::WalFileSnapshot snapshot;
    std::string name;
};

}  // namespace

Result<RecoveryReport> ScanWalDirectory(const std::filesystem::path& wal_directory,
                                        const InstanceId& instance_id,
                                        std::uint32_t dimension,
                                        Sequence checkpoint_sequence,
                                        const WalOptions& options,
                                        WalRecoveryLimits limits) {
    if (dimension == 0) return Error{ErrorCode::invalid_argument, "Recovery needs a dimension"};
    if (AllZero(instance_id))
        return Error{ErrorCode::invalid_argument, "Recovery needs an instance identity"};
    RecoveryReport report;
    auto workspace_left = limits.max_workspace_bytes;
    const auto charge = [&workspace_left](std::uint64_t bytes) {
        if (bytes > workspace_left) return false;
        workspace_left -= bytes;
        return true;
    };
    const auto no_workspace = [] {
        return Error{ErrorCode::resource_exhausted, "WAL recovery exceeds the workspace budget"};
    };

    std::error_code error;
    if (!std::filesystem::is_directory(wal_directory, error)) {
        // A fresh instance has no WAL yet; that is an empty history, not a fault.
        if (error && error != std::errc::no_such_file_or_directory)
            return Error{ErrorCode::io_error,
                         "Cannot inspect the WAL directory: " + wal_directory.string()};
        return report;
    }

    std::vector<WalFile> files;
    std::filesystem::directory_iterator entry(wal_directory, error), end;
    for (; !error && entry != end; entry.increment(error)) {
        const auto& path = entry->path();
        if (path.extension() != L".log") continue;
        if (files.size() >= limits.max_files)
            return Error{ErrorCode::resource_exhausted, "WAL history exceeds the recovery file limit"};
        // Include both file lists, transient vector growth, names, and path copies.
        const auto file_workspace = 4 * (sizeof(WalFile) + sizeof(WalFileSummary)) + 256 +
            4 * path.native().size() * sizeof(std::filesystem::path::value_type);
        if (!charge(file_workspace)) return no_workspace();
        auto snapshot = wal_detail::ReadWalHeaderForRecovery(path);
        if (const auto* failure = std::get_if<Error>(&snapshot)) return *failure;
        WalFile file;
        file.snapshot = std::get<wal_detail::WalFileSnapshot>(std::move(snapshot));
        auto header = DecodeWalFileHeader(file.snapshot.encoded_header);
        if (const auto* failure = std::get_if<Error>(&header)) return *failure;
        file.header = std::get<WalFileHeader>(header);
        file.name = path.filename().string();
        if (!SameInstance(file.header.instance_id, instance_id))
            return Corrupt("WAL file belongs to another instance: " + file.name);
        files.push_back(std::move(file));
    }
    if (error) return Error{ErrorCode::io_error, "Cannot list the WAL directory", error.value()};

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

    const auto frame_size = WalFrameBytes(dimension);
    if (const auto* failure = std::get_if<Error>(&frame_size)) return *failure;
    const auto bytes = std::get<std::uint64_t>(frame_size);
    if (bytes > options.max_frame_bytes)
        return Error{ErrorCode::resource_exhausted, "WAL frame exceeds the recovery frame limit"};
    if (!charge(bytes)) return no_workspace();
    std::vector<std::byte> buffer(static_cast<std::size_t>(bytes));
    Sequence expected = files.front().header.start_sequence;
    if (expected > checkpoint_sequence && expected - checkpoint_sequence != 1)
        return Corrupt("WAL history has a gap after the checkpoint");
    for (std::size_t file_index = 0; file_index < files.size(); ++file_index) {
        auto& file = files[file_index];
        if (file.header.start_sequence != expected) {
            // Failed reclamation may leave an old log beside a newer one.
            // Missing sequences are safe only when the manifest covers them.
            if (expected == 0 || file.header.start_sequence <= expected ||
                file.header.start_sequence - 1 > checkpoint_sequence)
                return Corrupt("WAL files do not continue each other at sequence " +
                               std::to_string(file.header.start_sequence));
            expected = file.header.start_sequence;
        }
        const bool is_final = file_index + 1 == files.size();
        WalFileSummary summary;
        summary.path = file.snapshot.path;
        summary.start_sequence = file.header.start_sequence;
        summary.file_bytes = file.snapshot.scanned_bytes;

        std::uint64_t offset = kHeaderBytes;
        const auto scanned = wal_detail::ScanWalFrames(
            file.snapshot, buffer, [&](std::span<const std::byte> content) -> Result<Success> {
            auto decoded = DecodeWalFrame(content, dimension, options.max_frame_bytes);
            if (const auto* failure = std::get_if<Error>(&decoded)) return *failure;
            const auto& frame = std::get<DecodedWalFrame>(decoded);
            if (frame.status == WalDecodeStatus::need_more) {
                // Only the newest file may end mid-frame. Rotation happens
                // between synchronised groups, so a short tail in an older file
                // means acknowledged bytes were lost.
                if (!is_final)
                    return Corrupt("Non-final WAL file ends inside a frame: " + file.name);
                if (!MatchesSequencePrefix(content, expected))
                    return Corrupt("Incomplete WAL frame does not match the next sequence");
                summary.discarded_tail_bytes = content.size();
                report.discarded_tail_bytes += summary.discarded_tail_bytes;
                return Success{};
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
                return Success{};
            }
            if (report.records.size() >= limits.max_records)
                return Error{ErrorCode::resource_exhausted,
                             "The WAL holds more records than the delta can retain"};
            // Retained payload plus vector growth and the replay-side ID set.
            const auto record_workspace = 4 * sizeof(ReplayedRecord) + 128 +
                static_cast<std::uint64_t>(dimension) * sizeof(float);
            if (!charge(record_workspace)) return no_workspace();
            ReplayedRecord record;
            record.operation = frame.operation;
            record.sequence = frame.sequence;
            record.id = frame.id;
            record.values.resize(dimension);
            std::memcpy(record.values.data(), frame.payload.data(),
                        static_cast<std::size_t>(dimension) * sizeof(float));
            report.records.push_back(std::move(record));
            return Success{};
        });
        if (const auto* failure = std::get_if<Error>(&scanned)) return *failure;
        summary.bytes_read = offset;
        if (summary.highest_sequence > report.highest_sequence)
            report.highest_sequence = summary.highest_sequence;
        if (is_final) {
            const auto recovered = (std::max)(report.highest_sequence, checkpoint_sequence);
            if (offset == kHeaderBytes &&
                (file.header.start_sequence <= recovered ||
                 file.header.start_sequence - recovered != 1))
                return Corrupt("Empty latest WAL does not start at the next sequence");
            report.latest = LatestWalRecovery{std::move(file.snapshot), file.header, offset};
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

    std::vector<std::pair<Sequence, std::filesystem::path>> covered;
    std::filesystem::directory_iterator entry(wal_directory, error), end;
    for (; !error && entry != end; entry.increment(error)) {
        const auto& path = entry->path();
        if (path.extension() != L".log") continue;
        WalFileHeader decoded{};
        bool usable = false;
        {
            std::ifstream input(path, std::ios::binary);
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
            // A live writer can deny read access to its own file. Leave
            // unverified files alone.
            if (failed != nullptr) ++*failed;
        } else if (decoded.start_sequence <= up_to) {
            covered.emplace_back(decoded.start_sequence, path);
        }
    }
    if (error) return Error{ErrorCode::io_error, "Cannot list the WAL directory", error.value()};

    std::sort(covered.begin(), covered.end());
    std::uint64_t removed = 0;
    // Only remove a prefix. If an older file is pinned, deleting later files
    // would leave a gap that the next recovery must reject.
    for (const auto& candidate : covered) {
        std::error_code remove_error;
        if (std::filesystem::remove(candidate.second, remove_error)) {
            ++removed;
        } else {
            if (failed != nullptr) ++*failed;
            break;
        }
    }
    return removed;
}

}  // namespace agent_io
