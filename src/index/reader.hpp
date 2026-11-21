#pragma once

// Reader-side plumbing: opens an index directory (CURRENT -> manifest ->
// segment.json -> files) and exposes the graph adapter used by Engine.

#include "agent_io/index.hpp"
#include "agent_io/storage.hpp"
#include "format.hpp"

#include <memory>
#include <vector>

namespace agent_io {

struct OpenedIndex {
    Manifest manifest;
    std::unique_ptr<IndexAdapter> adapter;
    // Bytes held outside the engine memory budget: manifest copy, open-time
    // metadata and the segment directory sparse index. Disclosed, never zero
    // silently.
    std::uint64_t unbudgeted_metadata_bytes = 0;
    std::uint64_t registered_metadata_bytes = 0;
};

// Point reads used by Engine::Get and by exact re-ranking. Kept separate from
// IndexAdapter so the frozen contract in index.hpp stays unchanged.
class VectorAccess {
public:
    virtual ~VectorAccess() = default;
    virtual Result<StoredVector> ReadVector(SegmentId segment, NativeNodeId node,
                                            Sequence snapshot) = 0;
    virtual Result<std::optional<VersionDirectoryEntry>> FindId(SegmentId segment,
                                                                VectorId id) = 0;
};

// Read-only description used for run artifacts. Evaluates the same metadata
// accounting as OpenIndexDirectory without registering files.
struct IndexDescription {
    Generation generation = 0;
    Sequence checkpoint_sequence = 0;
    std::uint64_t live_count = 0;
    std::uint64_t unbudgeted_metadata_bytes = 0;
    std::uint32_t dimension = 0;
    std::uint32_t segment_count = 0;
    std::string adapter_name;
};

Result<IndexDescription> DescribeIndex(const std::filesystem::path& directory);

// Verifies CURRENT and every declared file hash before returning an adapter.
Result<OpenedIndex> OpenIndexDirectory(
    const std::filesystem::path& directory,
    std::shared_ptr<BlockReader> reader,
    std::shared_ptr<FileRegistry> registry,
    std::uint32_t block_bytes);

}  // namespace agent_io
