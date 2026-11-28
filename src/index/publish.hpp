#pragma once

// Publishes a new manifest generation and switches CURRENT to it.
//
// The order is the contract: the manifest and every file it references must be
// durable before CURRENT names it. A crash before the switch leaves the old
// generation in charge; a crash after it leaves a manifest that is already
// complete. Nothing in between is observable.

#include "agent_io/index.hpp"

#include <filesystem>
#include <windows.h>

namespace agent_io {

struct PublishedManifest {
    Generation generation = 0;
    std::string manifest_path;
    Digest manifest_sha256{};
};

Result<Generation> NextManifestGeneration(const std::filesystem::path& directory,
                                          Generation current);
// The explicit checkpoint tests substitute a native rename failure on their caller thread.
decltype(&::MoveFileExW)& SegmentRename();
Result<PublishedManifest> PublishManifest(const std::filesystem::path& directory,
                                          const Manifest& manifest);

}  // namespace agent_io
