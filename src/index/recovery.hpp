#pragma once

// Stage 6 B-side: WAL recovery.
//
// A side owns the FORMAT-1 codec and the writer; deciding what a directory of
// files means is B's job. This module answers three questions without trusting
// a file name:
//
// * which files belong to this instance,
// * how far each one got, and whether an incomplete tail is legitimate,
// * which records still need replaying above the checkpoint sequence.
//
// The rules are deliberately asymmetric. The newest file may end in a partial
// frame: that is a write that was never acknowledged. The read-only scan
// records where the caller can repair it. Since rotation happens between
// synchronised groups, a short tail in an older file means durable bytes
// were lost.

#include "agent_io/engine.hpp"
#include "agent_io/index.hpp"
#include "agent_io/storage.hpp"
#include "../storage/wal_native.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace agent_io {

struct WalFileSummary {
    std::string name;
    Sequence start_sequence = 0;
    Sequence highest_sequence = 0;
    std::uint64_t frames = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t file_bytes = 0;
    // Bytes of the incomplete trailing frame that were discarded.
    std::uint64_t discarded_tail_bytes = 0;
};

struct ReplayedRecord {
    WalOperation operation = WalOperation::insert;
    Sequence sequence = 0;
    VectorId id = 0;
    std::vector<float> values;
};

struct LatestWalRecovery {
    wal_detail::WalFileSnapshot file;
    WalFileHeader header;
    std::uint64_t valid_bytes = 0;
};

struct RecoveryReport {
    std::vector<WalFileSummary> files;
    std::vector<ReplayedRecord> records;
    // Highest complete sequence found anywhere, whether or not it was replayed.
    Sequence highest_sequence = 0;
    std::uint64_t discarded_tail_bytes = 0;
    std::uint64_t ignored_checkpointed_records = 0;
    std::optional<LatestWalRecovery> latest;
};

// Scans `wal_directory` for files of `instance_id` and returns the records that
// still have to be applied above `checkpoint_sequence`. A missing directory is
// an empty history, not an error. Any ambiguity -- a foreign instance, a
// duplicate start sequence, a gap, a truncated non-final file -- is corruption.
Result<RecoveryReport> ScanWalDirectory(const std::filesystem::path& wal_directory,
                                        const InstanceId& instance_id,
                                        std::uint32_t dimension,
                                        Sequence checkpoint_sequence,
                                        const WalOptions& options);

}  // namespace agent_io
