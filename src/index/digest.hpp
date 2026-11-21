#pragma once

// Small helpers shared by the first build and the reader: content hashes,
// instance identifiers and the canonical textual forms used by FORMAT-1.

#include "agent_io/index.hpp"

#include <string>
#include <string_view>

namespace agent_io {

Result<Digest> HashFile(const std::filesystem::path& path);
Result<Digest> ParseDigest(std::string_view text);
std::string FormatDigest(const Digest& digest);

// RFC 4122 byte order as stored in the manifest.
InstanceId NewInstanceId();
std::string FormatUuid(const InstanceId& id);
Result<InstanceId> ParseUuid(std::string_view text);

bool IsAsciiPath(const std::filesystem::path& path);

}  // namespace agent_io
