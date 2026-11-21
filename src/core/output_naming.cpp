#include "core/output_naming.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace audio_converter {

namespace {

bool path_occupied(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(file_io_path(path), error);
    if (error && error != std::errc::no_such_file_or_directory) {
        throw fs::filesystem_error("Cannot inspect output path", path, error);
    }
    return status.type() != fs::file_type::not_found;
}

fs::path::string_type safe_stem(const fs::path& source) {
    auto stem = source.stem().native();
#ifdef _WIN32
    for (auto& character : stem) {
        if (character < 32 || std::wstring_view(L"<>:\"/\\|?*").find(character) !=
                                  std::wstring_view::npos) {
            character = L'_';
        }
    }
    while (!stem.empty() && (stem.back() == L'.' || stem.back() == L' ')) {
        stem.pop_back();
    }
    auto prefix = stem.substr(0, stem.find(L'.'));
    for (auto& character : prefix) {
        if (character >= L'a' && character <= L'z') {
            character -= L'a' - L'A';
        }
    }
    const bool numbered_device = prefix.size() == 4 &&
        (prefix.starts_with(L"COM") || prefix.starts_with(L"LPT")) &&
        ((prefix[3] >= L'1' && prefix[3] <= L'9') ||
         prefix[3] == L'\u00b9' || prefix[3] == L'\u00b2' || prefix[3] == L'\u00b3');
    if (prefix == L"CON" || prefix == L"PRN" || prefix == L"AUX" || prefix == L"NUL" ||
        prefix == L"CONIN$" || prefix == L"CONOUT$" || numbered_device) {
        stem.insert(prefix.size(), 1, L'_');
    }
#endif
    return stem.empty() ? fs::path("audio").native() : stem;
}

void shorten(fs::path::string_type& stem, std::size_t size) {
    if (stem.size() <= size) {
        return;
    }
#ifdef _WIN32
    if (size > 0 && stem[size - 1] >= 0xd800 && stem[size - 1] <= 0xdbff) {
        --size;
    }
#else
    while (size > 0 && (static_cast<unsigned char>(stem[size]) & 0xc0) == 0x80) {
        --size;
    }
#endif
    stem.resize(size);
}

fs::path mp3_filename(const fs::path& source, std::uint64_t suffix, std::size_t limit) {
    fs::path tail;
    if (suffix != 0) {
        tail += " (";
        tail += std::to_string(suffix);
        tail += ")";
    }
    tail += ".mp3";
    if (limit <= tail.native().size()) {
        throw std::length_error("Destination filename limit is too small");
    }
    auto stem = safe_stem(source);
    shorten(stem, limit - tail.native().size());
    if (stem.empty()) {
        stem = fs::path("_").native();
    }
    return fs::path(stem) += tail.native();
}

} // namespace

fs::path OutputPathPlanner::reserve(const OutputRequest& request) {
    const auto source = normalize_source_path(request.source);
    if (request.destination_directory && request.destination_directory->empty()) {
        throw std::invalid_argument("Custom output directory is empty");
    }
    const auto directory = normalize_directory(
        request.destination_directory.value_or(source.parent_path()));
    const auto limit = filename_limit(directory);
    std::lock_guard lock(mutex_);
    for (std::uint64_t suffix = 0; ; ++suffix) {
        const auto candidate = directory / mp3_filename(request.source.filename(), suffix, limit);
        if (!reserved_.contains(candidate) && !path_occupied(candidate)) {
            reserved_.insert(candidate);
            return candidate;
        }
        if (suffix == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Output filename suffixes exhausted");
        }
    }
}

void OutputPathPlanner::clear() {
    std::lock_guard lock(mutex_);
    reserved_.clear();
}

} // namespace audio_converter
