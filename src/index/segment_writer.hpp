#pragma once

// Writes one immutable FORMAT-1 segment from vectors already held in memory.
//
// The first build reads its input from files and owns the whole directory; a
// checkpoint only owns a staging directory and hands the result to the
// manifest publisher. Sharing the row encoder, layout and metadata form is what
// keeps a maintenance segment readable by the same adapter as the base segment.

#include "agent_io/index.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace agent_io {

struct SegmentRecord {
    VectorId id = 0;
    Sequence version = 0;
    std::vector<float> values;
};

struct SegmentWriteRequest {
    std::filesystem::path staging_directory;
    std::uint32_t dimension = 0;
    std::uint32_t block_bytes = 4096;
    NodeRecordLayout layout;
    // One row per logical ID. Callers resolve versions; the writer refuses
    // duplicates rather than guessing which one the version directory means.
    std::vector<SegmentRecord> records;
    std::string builder;
    std::string builder_version;
    std::string parameters;
};

struct SegmentArtifact {
    InstanceId uuid{};
    std::uint64_t node_count = 0;
    std::uint64_t graph_bytes = 0;
    Digest graph_sha256{};
    std::uint64_t version_logical_bytes = 0;
    std::uint64_t version_physical_bytes = 0;
    Digest version_sha256{};
    std::uint64_t metadata_bytes = 0;
    Digest metadata_sha256{};
    std::uint64_t staged_bytes = 0;
};

Result<SegmentArtifact> WriteSegment(const SegmentWriteRequest& request);

}  // namespace agent_io
