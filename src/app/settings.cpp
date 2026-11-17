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

} // namespace audio_converter
