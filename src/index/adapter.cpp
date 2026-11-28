#include "reader.hpp"

#include "digest.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace agent_io {
namespace {

constexpr std::uint32_t kPageEntries = 128;  // 4096 / 32
constexpr std::uint32_t kEntrySeeds = 16;
constexpr const char* kAdapterName = "agent_io_native_graph";
constexpr std::uint64_t kMaxMetadataBytes = 1u << 20;

Error Invalid(std::string message) {
    return Error{ErrorCode::invalid_argument, std::move(message)};
}

Error Corrupt(std::string message) {
    return Error{ErrorCode::corruption, std::move(message)};
}

Result<std::string> ReadSmallFile(const std::filesystem::path& path, std::uint64_t maximum) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return Error{ErrorCode::io_error, "Cannot stat index file: " + path.string()};
    if (size > maximum) return Corrupt("Index metadata file exceeds its bound: " + path.string());
    std::ifstream input(path, std::ios::binary);
    if (!input) return Error{ErrorCode::io_error, "Cannot open index file: " + path.string()};
    std::string text(static_cast<std::size_t>(size), '\0');
    if (size) input.read(text.data(), static_cast<std::streamsize>(size));
    if (!input) return Error{ErrorCode::io_error, "Cannot read index file: " + path.string()};
    return text;
}

std::uint64_t AlignedBytes(std::uint64_t logical, std::uint64_t block) {
    const auto remainder = logical % block;
    return remainder == 0 ? logical : logical + (block - remainder);
}

double SquaredL2(const float* left, const float* right, std::uint32_t dimension) {
    double total = 0.0;
    for (std::uint32_t index = 0; index < dimension; ++index) {
        const double delta = static_cast<double>(left[index]) - static_cast<double>(right[index]);
        total += delta * delta;
    }
    return total;
}

// Version directory with a bounded sparse index: one first-ID per block page,
// pages fetched through BlockReader on demand. Never loads the whole file.
class PagedVersionDirectory final : public VersionDirectory {
public:
    PagedVersionDirectory(std::shared_ptr<BlockReader> reader,
                          std::shared_ptr<const FileLease> file,
                          std::uint64_t entry_count, std::uint32_t block_bytes)
        : reader_(std::move(reader)), file_(std::move(file)), entries_(entry_count),
          block_bytes_(block_bytes) {}

    std::uint64_t EntryCount() const noexcept override { return entries_; }
    std::uint64_t PageIndexBytes() const noexcept {
        return static_cast<std::uint64_t>(first_ids_.size()) * sizeof(VectorId);
    }

    Result<Success> BuildIndex(std::chrono::steady_clock::time_point deadline) {
        if (entries_ == 0) return Corrupt("Empty version directory");
        const std::uint64_t page_bytes = block_bytes_;
        const std::uint64_t pages = (entries_ * kVersionRecordBytes + page_bytes - 1) / page_bytes;
        first_ids_.reserve(static_cast<std::size_t>(pages));
        for (std::uint64_t page = 0; page < pages; ++page) {
            auto block = ReadPage(page, deadline);
            if (const auto* error = std::get_if<Error>(&block)) return *error;
            const auto bytes = std::get<BlockLeasePtr>(block)->Bytes();
            const auto record = DecodeVersionRecord(bytes.first(kVersionRecordBytes));
            if (const auto* error = std::get_if<Error>(&record)) return *error;
            const auto value = std::get<VersionRecord>(record);
            if (!first_ids_.empty() && value.id <= first_ids_.back())
                return Corrupt("Version directory is not sorted by logical ID");
            first_ids_.push_back(value.id);
        }
        return Success{};
    }

    Result<std::optional<VersionDirectoryEntry>> Find(VectorId id) override {
        if (first_ids_.empty() || id < first_ids_.front()) return std::nullopt;
        const auto iterator = std::upper_bound(first_ids_.begin(), first_ids_.end(), id);
        const auto index = static_cast<std::uint64_t>(iterator - first_ids_.begin()) - 1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        auto block = ReadPage(index, deadline);
        if (const auto* error = std::get_if<Error>(&block)) return *error;
        const auto bytes = std::get<BlockLeasePtr>(block)->Bytes();
        const std::uint64_t first = index * kPageEntries;
        const std::uint64_t last = (std::min)(first + kPageEntries, entries_);
        for (std::uint64_t entry = first; entry < last; ++entry) {
            const auto record = DecodeVersionRecord(
                bytes.subspan(static_cast<std::size_t>((entry - first) * kVersionRecordBytes),
                              kVersionRecordBytes));
            if (const auto* error = std::get_if<Error>(&record)) return *error;
            const auto value = std::get<VersionRecord>(record);
            if (value.id == id) {
                VersionDirectoryEntry match;
                match.id = value.id;
                match.version = value.version;
                match.node = value.node;
                return std::optional<VersionDirectoryEntry>(match);
            }
            if (value.id > id) break;
        }
        return std::nullopt;
    }

private:
    Result<BlockLeasePtr> ReadPage(std::uint64_t page,
                                   std::chrono::steady_clock::time_point deadline) {
        const std::uint64_t offset = page * block_bytes_;
        if (offset >= file_->SizeBytes()) return Corrupt("Version directory page outside the file");
        if (block_bytes_ > file_->SizeBytes() - offset)
            return Corrupt("Version directory page crosses the file end");
        BlockLocation location{BlockKey{file_->Id(), offset, block_bytes_}, file_};
        const std::span<const BlockLocation> locations(&location, 1);
        auto lease = reader_->ReadBatch(locations, BlockReadOptions{ReadPriority::demand, deadline});
        if (const auto* error = std::get_if<Error>(&lease)) return *error;
        auto blocks = std::get<std::vector<BlockLeasePtr>>(std::move(lease));
        if (blocks.size() != 1) return Corrupt("Block reader returned an unexpected batch size");
        return blocks.front();
    }

    std::shared_ptr<BlockReader> reader_;
    std::shared_ptr<const FileLease> file_;
    std::uint64_t entries_ = 0;
    std::uint32_t block_bytes_ = 0;
    std::vector<VectorId> first_ids_;
};

struct SegmentState {
    SegmentDescriptor descriptor;
    SegmentMetadata metadata;
    std::shared_ptr<const FileLease> graph;
    std::shared_ptr<const FileLease> versions;
    std::unique_ptr<PagedVersionDirectory> directory;
};

struct Scored {
    double distance = 0.0;
    VectorId id = 0;
    Sequence version = 0;
    std::uint32_t node = 0;
};

struct FrontierEntry {
    double distance = 0.0;
    std::uint32_t node = 0;
    bool operator<(const FrontierEntry& other) const {
        return distance > other.distance;  // min heap
    }
};

class GraphAdapter final : public IndexAdapter, public VectorAccess {
public:
    GraphAdapter(IndexGeometry geometry, std::vector<SegmentState> segments,
                 std::shared_ptr<BlockReader> reader, std::uint32_t max_batch_blocks)
        : geometry_(geometry), segments_(std::move(segments)), reader_(std::move(reader)),
          max_batch_blocks_(max_batch_blocks) {}

    Result<IndexGeometry> Geometry() const override { return geometry_; }

    Result<NodeLocation> Locate(SegmentId segment, NativeNodeId node,
                                const ReadViewLease& view) const override {
        const auto* state = FindSegment(segment);
        if (state == nullptr) return Error{ErrorCode::not_found, "Segment is not in this index"};
        bool visible = false;
        for (std::uint32_t index = 0; index < view.view.segment_count; ++index)
            visible = visible || view.view.segments[index].id == segment;
        if (!visible) return Invalid("Segment is not part of the supplied view");
        if (state->descriptor.node_count == 0 ||
            node >= state->descriptor.node_count)
            return Invalid("Node id is outside the segment");
        const auto& layout = state->descriptor.layout;
        const std::uint64_t offset = static_cast<std::uint64_t>(node) * layout.row_bytes;
        if (layout.row_bytes != 0 && offset / layout.row_bytes != node)
            return Invalid("Node offset overflows");
        NodeLocation location;
        location.segment = segment;
        location.node = node;
        location.block_index = offset / state->descriptor.block_bytes;
        location.offset_in_block =
            static_cast<std::uint32_t>(offset % state->descriptor.block_bytes);
        location.bytes = layout.row_bytes;
        return location;
    }

    Result<ExpansionResult> Expand(const ExpansionRequest& request) override {
        if (request.k == 0 || request.k > request.budget.total_limit)
            return Invalid("Expand: k must be within the candidate budget");
        if (request.query.size() != geometry_.dimension)
            return Invalid("Expand: query dimension mismatch");
        const auto* query = request.query.data();
        ExpansionResult result;
        result.candidates.reserve(request.budget.total_limit);

        struct Merged {
            double distance;
            VectorId id;
            Sequence version;
            std::uint32_t node;
            SegmentId segment;
        };
        std::map<VectorId, Merged> merged;

        for (const auto& state : segments_) {
            bool visible = false;
            for (std::uint32_t index = 0; index < request.view.view.segment_count; ++index)
                visible = visible || request.view.view.segments[index].id == state.descriptor.id;
            if (!visible) continue;
            auto scored = SearchSegment(state, query, request, result);
            if (const auto* error = std::get_if<Error>(&scored)) return *error;
            auto candidates = std::get<std::vector<Scored>>(std::move(scored));
            if (candidates.size() > request.budget.per_segment_limit) {
                candidates.resize(request.budget.per_segment_limit);
                result.candidate_limit_reached = true;
            }
            for (const auto& candidate : candidates) {
                if (candidate.version > request.view.view.snapshot_sequence) continue;
                const auto existing = merged.find(candidate.id);
                if (existing == merged.end() ||
                    existing->second.version < candidate.version) {
                    merged[candidate.id] = Merged{candidate.distance, candidate.id,
                                                  candidate.version, candidate.node,
                                                  state.descriptor.id};
                }
            }
        }

        std::vector<Merged> ordered;
        ordered.reserve(merged.size());
        for (const auto& item : merged) ordered.push_back(item.second);
        std::sort(ordered.begin(), ordered.end(), [](const Merged& left, const Merged& right) {
            if (left.distance != right.distance) return left.distance < right.distance;
            return left.id < right.id;
        });
        if (ordered.size() > request.budget.total_limit) {
            ordered.resize(request.budget.total_limit);
            result.candidate_limit_reached = true;
        }
        for (const auto& item : ordered) {
            Candidate candidate;
            candidate.id = item.id;
            candidate.version = item.version;
            candidate.segment = item.segment;
            candidate.node = item.node;
            candidate.compressed_distance = item.distance;
            result.candidates.push_back(candidate);
        }
        return result;
    }

    Result<std::vector<Candidate>> Refine(const RefinementRequest& request) override {
        if (request.candidates.size() > request.budget.total_limit)
            return Invalid("Refine: candidate count exceeds the budget");
        if (request.query.size() != geometry_.dimension)
            return Invalid("Refine: query dimension mismatch");
        const auto deadline = request.deadline;
        std::map<VectorId, std::pair<Sequence, double>> best;
        for (const auto& candidate : request.candidates) {
            if (std::chrono::steady_clock::now() > deadline)
                return Error{ErrorCode::deadline_exceeded, "Refine deadline exceeded"};
            const auto* state = FindSegment(candidate.segment);
            if (state == nullptr) return Corrupt("Candidate segment is unknown");
            std::vector<float> values;
            const auto row = ReadNode(*state, candidate.node, deadline, values);
            if (const auto* error = std::get_if<Error>(&row)) return *error;
            const auto& node_row = std::get<NodeRow>(row);
            if (node_row.id != candidate.id)
                return Corrupt("Node row id disagrees with the version directory");
            if (node_row.version > request.view.view.snapshot_sequence)
                continue;
            const double distance = SquaredL2(request.query.data(), values.data(),
                                              geometry_.dimension);
            const auto existing = best.find(node_row.id);
            if (existing == best.end() || existing->second.first < node_row.version)
                best[node_row.id] = {node_row.version, distance};
        }
        std::vector<Candidate> output;
        output.reserve(best.size());
        for (const auto& item : best) {
            Candidate candidate;
            candidate.id = item.first;
            candidate.version = item.second.first;
            candidate.exact_distance = item.second.second;
            candidate.compressed_distance = item.second.second;
            output.push_back(candidate);
        }
        std::sort(output.begin(), output.end(), [](const Candidate& left, const Candidate& right) {
            const double left_distance = left.exact_distance.value_or(
                left.compressed_distance);
            const double right_distance = right.exact_distance.value_or(
                right.compressed_distance);
            if (left_distance != right_distance) return left_distance < right_distance;
            return left.id < right.id;
        });
        return output;
    }

    Result<StoredVector> ReadVector(SegmentId segment, NativeNodeId node,
                                   Sequence snapshot) override {
        const auto* state = FindSegment(segment);
        if (state == nullptr) return Error{ErrorCode::not_found, "Unknown segment"};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        std::vector<float> values;
        const auto row = ReadNode(*state, node, deadline, values);
        if (const auto* error = std::get_if<Error>(&row)) return *error;
        const auto& node_row = std::get<NodeRow>(row);
        if (node_row.version > snapshot)
            return Error{ErrorCode::not_found, "Vector is not visible at this snapshot"};
        StoredVector stored;
        stored.id = node_row.id;
        stored.version = node_row.version;
        stored.snapshot_sequence = snapshot;
        stored.values = values;
        return stored;
    }

    Result<std::optional<VersionDirectoryEntry>> FindId(SegmentId segment,
                                                       VectorId id) override {
        const auto* state = FindSegment(segment);
        if (state == nullptr) return Error{ErrorCode::not_found, "Unknown segment"};
        return state->directory->Find(id);
    }

    std::uint64_t DirectoryBytes() const noexcept {
        std::uint64_t total = 0;
        for (const auto& state : segments_)
            total += state.directory->PageIndexBytes();
        return total;
    }

    std::vector<FileId> SegmentFiles() const override {
        std::vector<FileId> files;
        files.reserve(segments_.size() * 2);
        for (const auto& state : segments_) {
            if (state.graph) files.push_back(state.graph->Id());
            if (state.versions) files.push_back(state.versions->Id());
        }
        return files;
    }

private:
    const SegmentState* FindSegment(SegmentId id) const {
        for (const auto& state : segments_)
            if (state.descriptor.id == id) return &state;
        return nullptr;
    }

    Result<NodeRow> ReadNode(const SegmentState& state, NativeNodeId node,
                             std::chrono::steady_clock::time_point deadline,
                             std::vector<float>& scratch) const {
        const auto& layout = state.descriptor.layout;
        const std::uint32_t block_bytes = state.descriptor.block_bytes;
        if (block_bytes == 0 || layout.row_bytes == 0 || layout.row_bytes % block_bytes != 0)
            return Corrupt("Segment layout cannot be read in whole blocks");
        const std::uint64_t offset = static_cast<std::uint64_t>(node) * layout.row_bytes;
        if (state.graph->SizeBytes() < offset ||
            layout.row_bytes > state.graph->SizeBytes() - offset)
            return Invalid("Node row is outside the segment file");
        const std::uint32_t blocks = layout.row_bytes / block_bytes;
        std::vector<BlockLocation> locations;
        locations.reserve(blocks);
        for (std::uint32_t index = 0; index < blocks; ++index) {
            BlockKey key{state.graph->Id(), offset + static_cast<std::uint64_t>(index) * block_bytes,
                         block_bytes};
            locations.push_back(BlockLocation{key, state.graph});
        }
        std::vector<std::byte> row(layout.row_bytes);
        for (std::size_t begin = 0; begin < blocks; begin += max_batch_blocks_) {
            const auto count = (std::min)(static_cast<std::size_t>(max_batch_blocks_), blocks - begin);
            auto leases = reader_->ReadBatch(std::span<const BlockLocation>(locations).subspan(begin, count),
                                             BlockReadOptions{ReadPriority::demand, deadline});
            if (const auto* error = std::get_if<Error>(&leases)) return *error;
            const auto& batch = std::get<std::vector<BlockLeasePtr>>(leases);
            if (batch.size() != count) return Corrupt("Short batch from the block reader");
            for (std::size_t index = 0; index < count; ++index) {
                const auto bytes = batch[index]->Bytes();
                if (bytes.size() < block_bytes) return Corrupt("Block lease is smaller than a block");
                std::memcpy(row.data() + (begin + index) * block_bytes, bytes.data(), block_bytes);
            }
        }
        return DecodeNodeRow(std::span<const std::byte>(row), layout, geometry_.dimension,
                             scratch);
    }

    Result<std::vector<Scored>> SearchSegment(const SegmentState& state, const float* query,
                                             const ExpansionRequest& request,
                                             ExpansionResult& counters) {
        const auto& layout = state.descriptor.layout;
        const std::uint32_t limit = request.budget.total_limit;
        const std::uint32_t per_segment = request.budget.per_segment_limit;
        const std::uint32_t working = (std::max)(per_segment, request.k);
        std::vector<Scored> results;
        std::priority_queue<FrontierEntry> frontier;
        std::unordered_map<std::uint32_t, float> visited;
        const std::size_t visited_cap =
            static_cast<std::size_t>(limit) * layout.max_degree + layout.max_degree + 1;
        visited.reserve(visited_cap);
        std::vector<float> scratch;
        if (state.descriptor.node_count == 0) return std::vector<Scored>{};

        // Best-first search prunes every frontier node whose distance exceeds the
        // working set's worst entry. With tight clusters that dead-ends inside the
        // entry cluster, because any cross-cluster hop is farther than the whole
        // local neighbourhood. A fixed deterministic spread of entry seeds (the
        // baseline stands in for hierarchical upper layers) keeps every region
        // reachable without inflating the visited set.
        std::vector<NativeNodeId> seeds;
        seeds.reserve(kEntrySeeds);
        const std::uint64_t count = state.descriptor.node_count;
        const std::uint64_t stride = 0x9E3779B97F4A7C15ull % count | 1ull;
        for (std::uint32_t index = 0; index < kEntrySeeds && seeds.size() < count; ++index) {
            const auto candidate = static_cast<NativeNodeId>((index * stride) % count);
            if (std::find(seeds.begin(), seeds.end(), candidate) != seeds.end()) continue;
            seeds.push_back(candidate);
        }
        if (state.descriptor.id == geometry_.entry_segment &&
            geometry_.entry_point < count &&
            std::find(seeds.begin(), seeds.end(), geometry_.entry_point) == seeds.end())
            seeds.push_back(geometry_.entry_point);

        std::vector<BlockLocation> seed_locations;
        seed_locations.reserve(seeds.size());
        for (const NativeNodeId seed : seeds) {
            const std::uint64_t offset = static_cast<std::uint64_t>(seed) * layout.row_bytes;
            if (state.graph->SizeBytes() < offset ||
                layout.row_bytes > state.graph->SizeBytes() - offset)
                return Invalid("Entry seed row is outside the segment file");
            seed_locations.push_back(BlockLocation{
                BlockKey{state.graph->Id(), offset, state.descriptor.block_bytes}, state.graph});
        }
        const auto score = [&](const BlockLease& block, NativeNodeId node) -> Result<Success> {
            const auto bytes = block.Bytes();
            if (bytes.size() < layout.row_bytes)
                return Corrupt("Graph block is smaller than a row");
            const auto decoded = DecodeNodeRow(bytes.first(layout.row_bytes), layout,
                                               geometry_.dimension, scratch);
            if (const auto* error = std::get_if<Error>(&decoded)) return *error;
            const auto& row = std::get<NodeRow>(decoded);
            const double distance = SquaredL2(query, scratch.data(), geometry_.dimension);
            const Scored candidate{distance, row.id, row.version, node};
            visited.emplace(node, static_cast<float>(distance));
            frontier.push(FrontierEntry{distance, node});
            if (results.size() < working) {
                results.push_back(candidate);
            } else {
                auto worst_iterator = std::max_element(results.begin(), results.end(),
                    [](const Scored& left, const Scored& right) {
                        return left.distance < right.distance;
                    });
                if (distance < worst_iterator->distance) *worst_iterator = candidate;
            }
            return Success{};
        };

        const auto read_and_score = [&](std::span<const BlockLocation> locations,
                                        std::span<const NativeNodeId> nodes,
                                        bool lookahead) -> Result<Success> {
            const auto width = (std::min)(std::size_t{8}, static_cast<std::size_t>(max_batch_blocks_));
            std::size_t admitted_for_batch = 0;
            for (std::size_t begin = 0; begin < locations.size(); begin += width) {
                const auto count = (std::min)(width, locations.size() - begin);
                auto leases = reader_->ReadBatch(locations.subspan(begin, count),
                    BlockReadOptions{ReadPriority::demand, request.deadline});
                if (const auto* error = std::get_if<Error>(&leases)) return *error;
                const auto& blocks = std::get<std::vector<BlockLeasePtr>>(leases);
                if (blocks.size() != count) return Corrupt("Short graph batch from the reader");
                counters.prefetched_keys_used += admitted_for_batch;
                admitted_for_batch = 0;
                // Start I/O for the next adjacent-neighbour batch before scoring this one.
                const auto next = begin + count;
                const auto window = (std::min)({width, locations.size() - next,
                    static_cast<std::size_t>(request.prefetch_blocks)});
                if (lookahead) {
                    for (const auto& location : locations.subspan(next, window)) {
                        auto hint = reader_->TryPrefetch(location, request.deadline);
                        const auto* disposition = std::get_if<PrefetchDisposition>(&hint);
                        if (disposition && *disposition == PrefetchDisposition::admitted)
                            ++admitted_for_batch;
                    }
                    counters.prefetch_admitted += admitted_for_batch;
                }
                counters.logical_blocks_read += blocks.size();
                for (std::size_t index = 0; index < count; ++index) {
                    counters.logical_bytes_read += blocks[index]->Key().byte_count;
                    auto scored = score(*blocks[index], nodes[begin + index]);
                    if (const auto* error = std::get_if<Error>(&scored)) return *error;
                }
            }
            return Success{};
        };
        auto seeded = read_and_score(seed_locations, seeds, false);
        if (const auto* error = std::get_if<Error>(&seeded)) return *error;

        while (!frontier.empty()) {
            if (std::chrono::steady_clock::now() > request.deadline) {
                if (results.size() >= request.k) {
                    counters.state = ExpansionState::more_available;
                } else {
                    return Error{ErrorCode::deadline_exceeded, "Expand deadline exceeded"};
                }
                break;
            }
            const auto current = frontier.top();
            const auto worst = results.empty() ? std::numeric_limits<double>::infinity() :
                std::max_element(results.begin(), results.end(),
                    [](const Scored& left, const Scored& right) {
                        return left.distance < right.distance;
                    })->distance;
            if (results.size() >= working && current.distance > worst) break;
            frontier.pop();
            auto row = ReadNode(state, current.node, request.deadline, scratch);
            if (const auto* error = std::get_if<Error>(&row)) return *error;
            counters.logical_blocks_read += layout.row_bytes / state.descriptor.block_bytes;
            counters.logical_bytes_read += layout.row_bytes;
            const auto& node_row = std::get<NodeRow>(row);
            const auto neighbours = node_row.neighbors;
            if (neighbours.empty()) continue;

            std::vector<BlockLocation> locations;
            locations.reserve(neighbours.size());
            std::vector<NativeNodeId> pending;
            pending.reserve(neighbours.size());
            for (const NativeNodeId neighbour : neighbours) {
                if (neighbour >= state.descriptor.node_count) continue;  // truncated edge
                if (visited.find(neighbour) != visited.end()) continue;
                if (visited.size() + pending.size() + 1 > visited_cap) {
                    counters.state = ExpansionState::budget_exhausted;
                    break;
                }
                pending.push_back(neighbour);
                const std::uint64_t offset =
                    static_cast<std::uint64_t>(neighbour) * layout.row_bytes;
                locations.push_back(BlockLocation{
                    BlockKey{state.graph->Id(), offset, state.descriptor.block_bytes},
                    state.graph});
            }
            if (pending.empty()) continue;
            auto scored = read_and_score(locations, pending, request.prefetch_blocks > 0);
            if (const auto* error = std::get_if<Error>(&scored)) return *error;
        }
        std::sort(results.begin(), results.end(), [](const Scored& left, const Scored& right) {
            if (left.distance != right.distance) return left.distance < right.distance;
            return left.id < right.id;
        });
        return results;
    }

    IndexGeometry geometry_;
    std::vector<SegmentState> segments_;
    std::shared_ptr<BlockReader> reader_;
    const std::uint32_t max_batch_blocks_;
};

}  // namespace

Result<OpenedIndex> OpenIndexDirectory(const std::filesystem::path& directory,
                                       std::shared_ptr<BlockReader> reader,
                                       std::shared_ptr<FileRegistry> registry,
                                       std::uint32_t block_bytes,
                                       std::uint32_t max_batch_blocks) {
    if (!reader || !registry || !max_batch_blocks)
        return Invalid("Open needs a block reader, file registry and nonzero batch capacity");
    auto current_text = ReadSmallFile(directory / "CURRENT", kMaxCurrentBytes);
    if (const auto* error = std::get_if<Error>(&current_text)) return *error;
    const auto current = DecodeCurrent(std::get<std::string>(current_text));
    if (const auto* error = std::get_if<Error>(&current)) return *error;
    const auto& record = std::get<CurrentRecord>(current);
    const auto manifest_path = (directory / record.manifest).lexically_normal();
    auto manifest_text = ReadSmallFile(manifest_path, kMaxManifestBytes);
    if (const auto* error = std::get_if<Error>(&manifest_text)) return *error;
    const auto& bytes = std::get<std::string>(manifest_text);
    {
        const auto hashed = HashFile(manifest_path);
        if (const auto* error = std::get_if<Error>(&hashed)) return *error;
        if (std::get<Digest>(hashed) != record.manifest_sha256)
            return Corrupt("CURRENT manifest hash does not match the file");
    }
    auto manifest = DecodeManifest(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
    if (const auto* error = std::get_if<Error>(&manifest)) return *error;
    Manifest decoded = std::get<Manifest>(manifest);
    if (std::memcmp(decoded.instance_id.data(), record.instance_id.data(),
                    decoded.instance_id.size()) != 0)
        return Corrupt("CURRENT instance id differs from the manifest");
    if (decoded.block_bytes != block_bytes)
        return Error{ErrorCode::unsupported,
                     "Configured block_bytes differs from the published manifest"};
    if (ManifestTextView(decoded.adapter.name) != kAdapterName)
        return Error{ErrorCode::unsupported,
                     "Unsupported index adapter: " + std::string(ManifestTextView(decoded.adapter.name))};

    IndexGeometry geometry;
    geometry.dimension = decoded.dimension;
    geometry.metric = decoded.metric;
    geometry.max_degree = decoded.layout.max_degree;
    geometry.entry_point = 0;
    geometry.entry_segment = decoded.segment_count ? decoded.segments[0].id : 0;
    geometry.pq_bytes_per_vector = 0;
    geometry.unbudgeted_metadata_bytes = 0;

    std::vector<SegmentState> segments;
    segments.reserve(decoded.segment_count);
    for (std::uint32_t index = 0; index < decoded.segment_count; ++index) {
        const auto& entry = decoded.segments[index];
        const auto segment_directory = directory / "segments" / FormatUuid(entry.uuid);
        const auto metadata_path = directory / std::string(ManifestTextView(entry.metadata.path));
        auto segment_text = ReadSmallFile(metadata_path, kMaxMetadataBytes);
        if (const auto* error = std::get_if<Error>(&segment_text)) return *error;
        {
            const auto hashed = HashFile(metadata_path);
            if (const auto* error = std::get_if<Error>(&hashed)) return *error;
            if (std::get<Digest>(hashed) != entry.metadata.sha256)
                return Corrupt("segment.json hash does not match the manifest");
        }
        auto metadata = DecodeSegmentMetadata(std::get<std::string>(segment_text));
        if (const auto* error = std::get_if<Error>(&metadata)) return *error;
        SegmentMetadata segment = std::get<SegmentMetadata>(metadata);
        if (std::memcmp(segment.uuid.data(), entry.uuid.data(), segment.uuid.size()) != 0)
            return Corrupt("segment.json uuid differs from the manifest");
        if (segment.dimension != decoded.dimension || segment.node_count != entry.node_count)
            return Corrupt("segment.json disagrees with the manifest about size or dimension");
        if (segment.layout.row_bytes != decoded.layout.row_bytes ||
            segment.layout.max_degree != decoded.layout.max_degree ||
            segment.layout.vector_offset != decoded.layout.vector_offset ||
            segment.layout.neighbor_offset != decoded.layout.neighbor_offset ||
            segment.layout.metadata_offset != decoded.layout.metadata_offset)
            return Error{ErrorCode::unsupported, "Segment layout differs from the manifest layout"};

        SegmentState state;
        state.metadata = std::move(segment);
        state.descriptor.id = entry.id;
        state.descriptor.generation = entry.generation;
        state.descriptor.kind = entry.kind;
        state.descriptor.covered_sequence = entry.covered_sequence;
        state.descriptor.node_count = entry.node_count;
        state.descriptor.file_bytes = entry.file_bytes;
        state.descriptor.block_bytes = decoded.block_bytes;
        state.descriptor.layout = decoded.layout;
        state.descriptor.has_version_directory = true;
        state.descriptor.content_sha256 = entry.content_sha256;

        for (const auto& file : state.metadata.files) {
            const auto path = segment_directory / file.path;
            std::error_code size_error;
            const auto size = std::filesystem::file_size(path, size_error);
            if (size_error) return Error{ErrorCode::io_error, "Missing segment file: " + file.path};
            if (size != file.physical_bytes)
                return Corrupt("Segment file length differs from segment.json: " + file.path);
            const auto hashed = HashFile(path);
            if (const auto* error = std::get_if<Error>(&hashed)) return *error;
            if (std::get<Digest>(hashed) != file.sha256)
                return Corrupt("Segment file hash mismatch: " + file.path);
            if (file.path == "graph.bin") {
                if (file.logical_bytes != entry.file_bytes ||
                    file.sha256 != entry.content_sha256)
                    return Corrupt("Manifest content hash disagrees with segment.json");
                const auto opened = registry->Open(path);
                if (const auto* error = std::get_if<Error>(&opened)) return *error;
                state.graph = std::get<std::shared_ptr<const FileLease>>(opened);
            } else if (file.path == state.metadata.version_directory_file) {
                const auto opened = registry->Open(path);
                if (const auto* error = std::get_if<Error>(&opened)) return *error;
                state.versions = std::get<std::shared_ptr<const FileLease>>(opened);
            }
        }
        if (!state.graph || !state.versions)
            return Corrupt("Segment is missing its graph or version directory file");
        state.directory = std::make_unique<PagedVersionDirectory>(
            reader, state.versions, state.metadata.version_entry_count, decoded.block_bytes);
        const auto built = state.directory->BuildIndex(
            std::chrono::steady_clock::now() + std::chrono::seconds(60));
        if (const auto* error = std::get_if<Error>(&built)) return *error;
        segments.push_back(std::move(state));
    }

    std::uint64_t directory_bytes = 0;
    std::uint64_t metadata_bytes = sizeof(Manifest);
    for (const auto& state : segments) {
        directory_bytes += state.directory->PageIndexBytes();
        metadata_bytes += state.directory->PageIndexBytes();
        for (const auto& file : state.metadata.files)
            metadata_bytes += file.path.size() + sizeof(SegmentFileRecord);
    }
    auto adapter = std::make_unique<GraphAdapter>(geometry, std::move(segments), reader,
                                                   max_batch_blocks);
    OpenedIndex opened;
    opened.registered_metadata_bytes = directory_bytes;
    opened.unbudgeted_metadata_bytes = metadata_bytes;
    opened.adapter = std::move(adapter);
    opened.manifest = decoded;
    return opened;
}

Result<IndexDescription> DescribeIndex(const std::filesystem::path& directory) {
    auto current_text = ReadSmallFile(directory / "CURRENT", kMaxCurrentBytes);
    if (const auto* error = std::get_if<Error>(&current_text)) return *error;
    const auto current = DecodeCurrent(std::get<std::string>(current_text));
    if (const auto* error = std::get_if<Error>(&current)) return *error;
    const auto& record = std::get<CurrentRecord>(current);
    auto manifest_text = ReadSmallFile(directory / record.manifest, kMaxManifestBytes);
    if (const auto* error = std::get_if<Error>(&manifest_text)) return *error;
    const auto& bytes = std::get<std::string>(manifest_text);
    auto manifest = DecodeManifest(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
    if (const auto* error = std::get_if<Error>(&manifest)) return *error;
    const Manifest decoded = std::get<Manifest>(manifest);
    IndexDescription description;
    description.generation = decoded.generation;
    description.checkpoint_sequence = decoded.checkpoint_sequence;
    description.live_count = decoded.live_count;
    description.dimension = decoded.dimension;
    description.segment_count = decoded.segment_count;
    description.adapter_name = std::string(ManifestTextView(decoded.adapter.name));
    std::uint64_t metadata_bytes = sizeof(Manifest);
    for (std::uint32_t index = 0; index < decoded.segment_count; ++index) {
        const auto& entry = decoded.segments[index];
        const auto path = directory / std::string(ManifestTextView(entry.metadata.path));
        auto segment_text = ReadSmallFile(path, kMaxMetadataBytes);
        if (const auto* error = std::get_if<Error>(&segment_text)) return *error;
        auto metadata = DecodeSegmentMetadata(std::get<std::string>(segment_text));
        if (const auto* error = std::get_if<Error>(&metadata)) return *error;
        const auto& segment = std::get<SegmentMetadata>(metadata);
        const std::uint64_t pages = (segment.version_entry_count * kVersionRecordBytes +
            decoded.block_bytes - 1) / decoded.block_bytes;
        metadata_bytes += pages * sizeof(VectorId);
        for (const auto& file : segment.files)
            metadata_bytes += file.path.size() + sizeof(SegmentFileRecord);
    }
    description.unbudgeted_metadata_bytes = metadata_bytes;
    return description;
}

}  // namespace agent_io
