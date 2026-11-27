#include "publish.hpp"

#include "digest.hpp"
#include "format.hpp"

#include <windows.h>

#include <fstream>
#include <string>
#include <vector>

namespace agent_io {
namespace {

Error Failure(ErrorCode code, std::string message, int system_error = 0) {
    return Error{code, std::move(message), system_error};
}

bool WriteWhole(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    if (!bytes.empty())
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    output.flush();
    return static_cast<bool>(output);
}

bool FlushPath(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool flushed = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    return flushed;
}

}  // namespace

Result<PublishedManifest> PublishManifest(const std::filesystem::path& directory,
                                          const Manifest& manifest) {
    if (directory.empty())
        return Failure(ErrorCode::invalid_argument, "Publish needs an index directory");
    if (manifest.generation == 0)
        return Failure(ErrorCode::invalid_argument, "Manifest generation must be positive");
    if (manifest.segment_count == 0 || manifest.segment_count > kMaxManifestSegments)
        return Failure(ErrorCode::invalid_argument, "Manifest segment count is out of range");

    std::error_code path_error;
    const auto manifests_directory = directory / "manifests";
    std::filesystem::create_directories(manifests_directory, path_error);
    if (path_error) return Failure(ErrorCode::io_error, "Cannot create the manifests directory");

    const auto manifest_size = ManifestBytes(manifest);
    if (const auto* error = std::get_if<Error>(&manifest_size)) return *error;
    std::vector<std::byte> bytes(static_cast<std::size_t>(
        std::get<std::uint64_t>(manifest_size)));
    const auto encoded = EncodeManifest(manifest, bytes);
    if (const auto* error = std::get_if<Error>(&encoded)) return *error;

    const auto final_path = manifests_directory /
        ("manifest-" + std::to_string(manifest.generation) + ".json");
    if (std::filesystem::exists(final_path, path_error))
        return Failure(ErrorCode::already_exists,
                       "Manifest generation already exists; generation must increase");
    const auto temp_path = manifests_directory /
        ("manifest-" + std::to_string(manifest.generation) + ".json.tmp");
    if (!WriteWhole(temp_path, bytes))
        return Failure(ErrorCode::io_error, "Cannot write the manifest");
    if (!FlushPath(temp_path))
        return Failure(ErrorCode::io_error, "Cannot flush the manifest",
                       static_cast<int>(GetLastError()));
    if (!MoveFileExW(temp_path.c_str(), final_path.c_str(), MOVEFILE_WRITE_THROUGH))
        return Failure(ErrorCode::io_error, "Cannot publish the manifest",
                       static_cast<int>(GetLastError()));
    const auto digest = HashFile(final_path);
    if (const auto* error = std::get_if<Error>(&digest)) return *error;

    CurrentRecord current;
    current.instance_id = manifest.instance_id;
    current.manifest = "manifests/manifest-" + std::to_string(manifest.generation) + ".json";
    current.manifest_sha256 = std::get<Digest>(digest);
    const auto current_text = EncodeCurrent(current);
    if (const auto* error = std::get_if<Error>(&current_text)) return *error;
    const auto& text = std::get<std::string>(current_text);
    const auto current_temp = directory / "CURRENT.tmp";
    if (!WriteWhole(current_temp, std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(text.data()), text.size())))
        return Failure(ErrorCode::io_error, "Cannot write CURRENT");
    if (!FlushPath(current_temp))
        return Failure(ErrorCode::io_error, "Cannot flush CURRENT",
                       static_cast<int>(GetLastError()));
    const auto current_path = directory / "CURRENT";
    const bool existed = std::filesystem::exists(current_path, path_error);
    bool published = false;
    if (existed) {
        const auto backup = directory / ("CURRENT.bak-" + FormatUuid(NewInstanceId()));
        published = ReplaceFileW(current_path.c_str(), current_temp.c_str(), backup.c_str(),
                                 REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != 0;
        if (!published) {
            std::error_code ignored;
            std::filesystem::remove(backup, ignored);
        }
    } else {
        published = MoveFileExW(current_temp.c_str(), current_path.c_str(),
                                MOVEFILE_WRITE_THROUGH) != 0;
    }
    if (!published)
        return Failure(ErrorCode::io_error,
                       "Cannot switch CURRENT; the previous generation is unchanged",
                       static_cast<int>(GetLastError()));

    PublishedManifest result;
    result.generation = manifest.generation;
    result.manifest_path = final_path.string();
    result.manifest_sha256 = std::get<Digest>(digest);
    return result;
}

}  // namespace agent_io
