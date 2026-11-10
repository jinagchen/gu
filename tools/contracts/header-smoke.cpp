#include "../../include/agent_io/engine.hpp"
#include "../../include/agent_io/storage.hpp"

#include <type_traits>
#include <utility>

using namespace agent_io;

static_assert(sizeof(VectorId) == 8);
static_assert(sizeof(Sequence) == 8);
static_assert(!std::is_copy_constructible_v<Engine>);
static_assert(std::is_abstract_v<ReadBackend>);
static_assert(std::is_abstract_v<WalWriter>);
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
