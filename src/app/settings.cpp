#include "app/settings.hpp"
#include "app/file_io.hpp"
#include "audio/process.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

namespace audio_converter {
namespace {

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return std::string(value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1));
}

unsigned integer(const std::string& text) {
    unsigned value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::invalid_argument("Expected a nonnegative integer");
    }
    return value;
}

bool boolean(const std::string& text) {
    if (text == "true" || text == "1") return true;
    if (text == "false" || text == "0") return false;
    throw std::invalid_argument("Expected true or false");
}

fs::path utf8_path(const std::string& value) {
    if (value.find('\0') != std::string::npos || value.find('\n') != std::string::npos ||
        value.find('\r') != std::string::npos) throw std::invalid_argument("Invalid directory path");
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

std::string read_ini(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open settings file");
    std::string data;
    char buffer[4096];
    while (stream.read(buffer, sizeof buffer) || stream.gcount()) {
        data.append(buffer, static_cast<std::size_t>(stream.gcount()));
        if (data.size() > 1024 * 1024) throw std::runtime_error("Settings exceed 1 MiB");
    }
    if (stream.bad()) throw std::runtime_error("Cannot read settings file");
    return data;
}

} // namespace

void validate_settings(const AppSettings& settings) {
    validate_settings(settings.audio);
    if (settings.workers > maximum_worker_override) throw AudioError({"invalid_workers", {}, {}});
    if (settings.output_mode != OutputMode::SameAsSource &&
        settings.output_mode != OutputMode::CustomDirectory) {
        throw AudioError({"invalid_output_mode", {}, {}});
    }
    if (settings.output_mode == OutputMode::CustomDirectory &&
        settings.custom_output_directory.empty()) throw AudioError({"invalid_output_directory", {}, {}});
    (void)utf8_path(path_utf8(settings.custom_output_directory));
}

std::size_t worker_count(unsigned requested, unsigned logical_processors, std::size_t jobs) {
    if (requested > maximum_worker_override) throw AudioError({"invalid_workers", {}, {}});
    const auto count = requested ? requested :
        logical_processors ? std::max(1U, logical_processors / 2) : 2U;
    return std::min<std::size_t>(count, jobs);
}

SettingsResult parse_settings(std::string_view ini, const fs::path& base_directory) {
    SettingsResult result;
    if (ini.size() > 1024 * 1024) {
        result.warnings.push_back({"settings_read_failed", {}, "Settings exceed 1 MiB"});
        return result;
    }
    if (ini.starts_with("\xef\xbb\xbf")) ini.remove_prefix(3);
    std::istringstream lines{std::string(ini)};
    std::set<std::string> seen;
    std::string section;
    unsigned line_number = 0;
    for (std::string line; std::getline(lines, line);) {
        ++line_number;
        line = trim(line);
        if (line.empty() || line.starts_with(';') || line.starts_with('#')) continue;
        auto warn = [&](std::string detail) {
            result.warnings.push_back({"settings_invalid_entry", {{"line", std::to_string(line_number)}},
                                       std::move(detail)});
        };
        if (line.front() == '[' && line.back() == ']') {
            section = trim(std::string_view(line).substr(1, line.size() - 2));
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos || line.find('\0') != std::string::npos) {
            warn("Expected key=value"); continue;
        }
        const auto key = section + "." + trim(std::string_view(line).substr(0, separator));
        auto value = trim(std::string_view(line).substr(separator + 1));
        if (!seen.insert(key).second) warn("Repeated key: " + key);
        try {
            if (key == "conversion.bitrate_kbps") {
                const auto rate = integer(value);
                auto audio = result.settings.audio;
                if (rate > 320) throw AudioError({"invalid_bitrate", {}, {}});
                audio.bitrate_kbps = static_cast<int>(rate);
                validate_settings(audio);
                result.settings.audio = audio;
            } else if (key == "conversion.channel_mode") {
                if (value == "auto") result.settings.audio.channel_mode = ChannelMode::Auto;
                else if (value == "stereo") result.settings.audio.channel_mode = ChannelMode::Stereo;
                else if (value == "mono") result.settings.audio.channel_mode = ChannelMode::Mono;
                else throw AudioError({"invalid_channels", {}, {}});
            } else if (key == "conversion.output_mode") {
                if (value == "same_as_source") result.settings.output_mode = OutputMode::SameAsSource;
                else if (value == "custom_directory") result.settings.output_mode = OutputMode::CustomDirectory;
                else throw AudioError({"invalid_output_mode", {}, {}});
            } else if (key == "conversion.custom_output_directory") {
                if (value.starts_with('"')) {
                    std::istringstream quoted(value);
                    if (!(quoted >> std::quoted(value)) || !(quoted >> std::ws).eof()) {
                        throw std::invalid_argument("Malformed quoted path");
                    }
                }
                auto path = utf8_path(value);
                if (!path.empty() && path.is_relative()) path = fs::absolute(base_directory / path);
                result.settings.custom_output_directory = path.lexically_normal();
            } else if (key == "conversion.force_upsampling") {
                result.settings.audio.force_upsampling = boolean(value);
            } else if (key == "import.include_mp3_from_folders") {
                result.settings.audio.include_mp3_from_folders = boolean(value);
            } else if (key == "import.recursive_folders") {
                result.settings.recursive_folders = boolean(value);
            } else if (key == "processing.workers") {
                const auto count = value == "auto" ? 0 : integer(value);
                if (count > maximum_worker_override) throw AudioError({"invalid_workers", {}, {}});
                result.settings.workers = count;
            } else if (key == "logging.enabled") result.settings.logging_enabled = boolean(value);
            else warn("Unknown key: " + key);
        } catch (const std::exception& error) { warn(key + ": " + error.what()); }
    }
    if (result.settings.output_mode == OutputMode::CustomDirectory &&
        result.settings.custom_output_directory.empty()) {
        result.settings.output_mode = OutputMode::SameAsSource;
        result.warnings.push_back({"invalid_output_directory", {}, {}});
    }
    return result;
}

std::string serialize_settings(const AppSettings& settings) {
    validate_settings(settings);
    std::ostringstream out;
    out << std::boolalpha << "[conversion]\nbitrate_kbps=" << settings.audio.bitrate_kbps
        << "\nchannel_mode=" << (settings.audio.channel_mode == ChannelMode::Mono ? "mono" :
                                   settings.audio.channel_mode == ChannelMode::Stereo ? "stereo" : "auto")
        << "\noutput_mode=" << (settings.output_mode == OutputMode::SameAsSource ?
                                  "same_as_source" : "custom_directory")
        << "\ncustom_output_directory=" << std::quoted(path_utf8(settings.custom_output_directory))
        << "\nforce_upsampling=" << settings.audio.force_upsampling
        << "\n\n[import]\ninclude_mp3_from_folders=" << settings.audio.include_mp3_from_folders
        << "\nrecursive_folders=" << settings.recursive_folders
        << "\n\n[processing]\nworkers=" << (settings.workers ? std::to_string(settings.workers) : "auto")
        << "\n\n[logging]\nenabled=" << settings.logging_enabled << '\n';
    return out.str();
}

} // namespace audio_converter
