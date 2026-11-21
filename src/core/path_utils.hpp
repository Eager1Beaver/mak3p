#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace audio_converter {

namespace fs = std::filesystem;

// Produces an absolute input path, resolving accessible filesystem aliases.
[[nodiscard]] fs::path normalize_source_path(const fs::path& path);

// Resolves an existing destination directory to its canonical absolute path.
[[nodiscard]] fs::path normalize_directory(const fs::path& directory);

struct PathLess {
    [[nodiscard]] bool operator()(const fs::path& left, const fs::path& right) const;
};

[[nodiscard]] std::size_t filename_limit(const fs::path& directory);

// Returns a path suitable for filesystem calls and media tools.
[[nodiscard]] fs::path file_io_path(const fs::path& path);

#ifdef _WIN32
[[nodiscard]] std::wstring windows_api_path(const fs::path& path);
#endif

} // namespace audio_converter
