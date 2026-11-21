#pragma once

// First build: consumes raw float32 vectors plus uint64 IDs, produces the
// project-native graph segment and publishes CURRENT through the staging
// protocol. Offline builds are expected to run inside their own Job.

#include "agent_io/index.hpp"

#include <filesystem>
#include <string>

namespace agent_io {

struct FirstBuildRequest {
    std::filesystem::path vectors_path;
    std::filesystem::path ids_path;
    std::string vectors_sha256;
    std::string ids_sha256;
    std::uint32_t dimension = 0;
    std::uint32_t max_degree = 32;
    std::uint32_t build_beam = 4;
    std::uint64_t vector_count = 0;
    std::uint64_t memory_limit_bytes = 0;
    std::uint64_t min_free_disk_bytes = 0;
    std::filesystem::path staging_directory;
    std::filesystem::path output_directory;
    std::string build_id;
    std::string native_parameters;
    std::string adapter_name = "agent_io_native_graph";
    std::string adapter_version = "1.0.0";
    // Version of the native library the adapter sits on (DiskANN, for example).
    // Distinct from adapter_version: the manifest must name both, because a
    // graph is only reproducible if the builder code *and* its base agree.
    std::string native_version;
    Sequence checkpoint_sequence = 0;
    Generation generation = 1;
    bool publish_current = true;
};

Result<BuildOutcome> RunFirstBuild(const FirstBuildRequest& request);

}  // namespace agent_io
