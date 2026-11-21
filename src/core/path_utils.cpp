#include "core/path_utils.hpp"

#include <cerrno>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace audio_converter {

namespace {

void validate_path(const fs::path& path) {
    if (path.native().find(fs::path::value_type{}) != fs::path::string_type::npos) {
        throw std::invalid_argument("A path contains an embedded NUL");
    }
}

fs::path preferred_path(fs::path path) {
#ifdef _WIN32
    auto native = path.native();
    if (native.starts_with(L"\\\\?\\UNC\\")) {
        path = L"\\\\" + native.substr(8);
    } else if (native.starts_with(L"\\\\?\\") && native.size() > 5 && native[5] == L':') {
        path = native.substr(4);
    }
#endif
    return path.lexically_normal().make_preferred();
}

} // namespace

fs::path normalize_source_path(const fs::path& path) {
    validate_path(path);
    if (path.empty() || path.filename().empty() ||
        path.filename() == "." || path.filename() == "..") {
        throw std::invalid_argument("Source file path is missing a filename");
    }
#ifdef _WIN32
    if (path.filename().native().find(L':') != std::wstring::npos) {
        throw std::invalid_argument("Source file path specifies a Windows alternate data stream");
    }
#endif
    const auto absolute = fs::absolute(path);
    std::error_code error;
    auto normalized = fs::weakly_canonical(file_io_path(absolute), error);
    return preferred_path(error ? absolute : normalized);
}

fs::path normalize_directory(const fs::path& directory) {
    validate_path(directory);
    const auto requested = directory.empty() ? fs::current_path() : directory;
    const auto native = file_io_path(requested);
    std::error_code error;
    const bool is_directory = fs::is_directory(native, error);
    if (error && error != std::errc::no_such_file_or_directory &&
        error != std::errc::not_a_directory) {
        throw fs::filesystem_error("Cannot inspect output directory", requested, error);
    }
    if (!is_directory) {
        throw std::invalid_argument("Output directory does not exist or is not a directory");
    }
    return preferred_path(fs::canonical(native));
}

bool PathLess::operator()(const fs::path& left, const fs::path& right) const {
#ifdef _WIN32
    const int comparison = CompareStringOrdinal(
        left.c_str(), -1, right.c_str(), -1, TRUE);
    if (comparison == 0) {
        throw std::system_error(
            static_cast<int>(GetLastError()), 
            std::system_category(),
            "Cannot compare paths");
        }
    return comparison == CSTR_LESS_THAN;
#else
    return left.native() < right.native();
#endif
}

std::size_t filename_limit(const fs::path& directory) {
#ifdef _WIN32
    (void)directory;
    return 255;
#else
    errno = 0;
    const long limit = ::pathconf(directory.c_str(), _PC_NAME_MAX);
    if (limit < 0 && errno != 0) {
        throw fs::filesystem_error(
            "Cannot inspect filename limit", 
            directory,
            std::error_code(errno, std::generic_category())
        );
    }
    return limit < 0 ? 255 : static_cast<std::size_t>(limit);
#endif
}

fs::path file_io_path(const fs::path& path) {
#ifdef _WIN32
    if (!path.empty()) {
        const auto absolute = path.is_absolute() ? path : fs::absolute(path);
        // Windows directory operations can require space for an appended 8.3 name.
        if (absolute.native().size() >= MAX_PATH - 12) {
            return windows_api_path(absolute);
        }
    }
#endif
    return path;
}

#ifdef _WIN32
std::wstring windows_api_path(const fs::path& path) {
    auto native = fs::absolute(path).lexically_normal().make_preferred().native();
    if (native.starts_with(L"\\\\?\\")) {
        return native;
    }
    if (native.starts_with(L"\\\\")) {
        return L"\\\\?\\UNC\\" + native.substr(2);
    }
    return L"\\\\?\\" + native;
}
#endif

} // namespace audio_converter
