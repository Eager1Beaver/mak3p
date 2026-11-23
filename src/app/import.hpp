#pragma once

#include "audio/audio.hpp"

namespace audio_converter {
[[nodiscard]] fs::path path_from_utf8(std::string_view text);
[[nodiscard]] std::vector<fs::path> dropped_paths(std::string_view payload);
[[nodiscard]] bool has_audio_file_extension(const fs::path& path);
[[nodiscard]] std::string audio_file_glob();
struct ImportResult {
    std::vector<SourceFile> files;
    std::vector<Diagnostic> warnings;
};
[[nodiscard]] ImportResult discover_sources(const std::vector<fs::path>& paths,
                                          bool recursive, std::stop_token stop = {});
[[nodiscard]] EnginePaths discover_engine(const fs::path& application_directory,
                                         const fs::path& selected_directory = {});
} // namespace audio_converter
