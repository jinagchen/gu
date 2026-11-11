#include "../../include/agent_io/engine.hpp"
#include "../../include/agent_io/storage.hpp"
#include "../../include/agent_io/benchmark.hpp"

#include <type_traits>
#include <utility>

using namespace agent_io;

static_assert(sizeof(VectorId) == 8);
static_assert(sizeof(Sequence) == 8);
static_assert(!std::is_copy_constructible_v<Engine>);
static_assert(std::is_abstract_v<ReadBackend>);
static_assert(std::is_abstract_v<WalWriter>);
static_assert(std::is_abstract_v<BlockReader>);
static_assert(std::is_abstract_v<FileRegistry>);
static_assert(std::is_abstract_v<ResourceGovernor>);
static_assert(std::is_same_v<
              decltype(std::declval<const BlockLease&>().Bytes()),
              std::span<const std::byte>>);
static_assert(std::is_same_v<
              decltype(std::declval<BlockReader&>().ReadBatch(
                  std::declval<std::span<const BlockLocation>>(),
                  std::declval<const BlockReadOptions&>())),
              Result<std::vector<BlockLeasePtr>>>);
static_assert(std::is_same_v<
              decltype(DecodeWalFrame(
                  std::declval<std::span<const std::byte>>(), 32, 1048576)),
              Result<DecodedWalFrame>>);
static_assert(std::is_same_v<decltype(RequestTiming::api_start_ns),
                             std::optional<std::uint64_t>>);
static_assert(IoBackend::synchronous != IoBackend::windows_iocp);
static_assert(std::is_same_v<
              decltype(std::declval<Engine&>().Search(
                  std::declval<std::span<const float>>(),
                  std::declval<const SearchOptions&>())),
              Result<SearchResult>>);
static_assert(std::is_same_v<
              decltype(std::declval<Engine&>().Upsert(
                  VectorId{}, std::declval<std::span<const float>>(),
                  std::declval<const WriteOptions&>())),
              Result<WriteReceipt>>);
