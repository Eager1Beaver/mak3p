#pragma once

#include "audio/audio.hpp"

#include <string_view>

namespace audio_converter {

enum class OutputMode { SameAsSource, CustomDirectory };

struct AppSettings {
    AudioSettings audio;
    OutputMode output_mode = OutputMode::SameAsSource;
    fs::path custom_output_directory;
    bool recursive_folders = true;
    unsigned workers = 0;
    bool logging_enabled = false;
    bool operator==(const AppSettings&) const = default;
};

inline constexpr unsigned maximum_worker_override = 64;
void validate_settings(const AppSettings& settings);
[[nodiscard]] std::size_t worker_count(unsigned requested, unsigned logical_processors,
                                      std::size_t runnable_jobs);

struct SettingsResult {
    AppSettings settings;
    std::vector<Diagnostic> warnings;
};

[[nodiscard]] SettingsResult parse_settings(std::string_view ini, const fs::path& base_directory);
[[nodiscard]] SettingsResult read_settings(const fs::path& path);
[[nodiscard]] std::string serialize_settings(const AppSettings& settings);
[[nodiscard]] fs::path executable_directory();
[[nodiscard]] fs::path default_settings_directory();

struct SettingsLoadResult : SettingsResult {
    fs::path path;
    bool using_fallback = false;
};

class SettingsStore {
public:
    explicit SettingsStore(fs::path application_directory,
                           fs::path fallback_directory = default_settings_directory());
    [[nodiscard]] SettingsLoadResult load();
    // Returns warnings rather than disrupting conversion if persistence fails.
    [[nodiscard]] std::vector<Diagnostic> save(const AppSettings& settings);
    [[nodiscard]] const fs::path& path() const noexcept { return selected_; }
private:
    fs::path application_directory_;
    fs::path fallback_directory_;
    fs::path selected_;
    bool fallback_ = false;
    void select();
};

} // namespace audio_converter
