#pragma once

#include "core/path_utils.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace test {

namespace fs = std::filesystem;

inline void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<class Exception = std::exception, class Function>
void throws(Function&& function, const std::string& message) {
    try {
        std::forward<Function>(function)();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

struct TempDirectory {
    fs::path path;

    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = fs::temp_directory_path() /
                ("audio-converter-test-" + std::to_string(stamp) + "-" + std::to_string(attempt));
            std::error_code error;
            if (fs::create_directory(candidate, error)) {
                path = fs::canonical(candidate);
                return;
            }
            if (error) {
                throw fs::filesystem_error("Cannot create test directory", candidate, error);
            }
        }
        throw std::runtime_error("Cannot create unique test directory");
    }

    ~TempDirectory() {
        std::error_code ignored;
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::add, ignored);
#ifdef _WIN32
        fs::remove_all(fs::path(audio_converter::windows_api_path(path)), ignored);
#else
        fs::remove_all(path, ignored);
#endif
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
};

struct WorkingDirectory {
    fs::path previous = fs::current_path();

    explicit WorkingDirectory(const fs::path& path) { fs::current_path(path); }
    ~WorkingDirectory() {
        std::error_code ignored;
        fs::current_path(previous, ignored);
    }

    WorkingDirectory(const WorkingDirectory&) = delete;
    WorkingDirectory& operator=(const WorkingDirectory&) = delete;
};

inline void write(const fs::path& path, const std::string& bytes) {
    std::ofstream stream;
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    stream.open(audio_converter::file_io_path(path), std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
}

inline std::string read(const fs::path& path) {
    std::ifstream stream(audio_converter::file_io_path(path), std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot read test file");
    }
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

using Case = std::pair<std::string, std::function<void()>>;

inline int run(int argc, char** argv, const std::vector<Case>& cases) {
    unsigned passed = 0;
    for (const auto& [name, function] : cases) {
        if (argc > 1 && name != argv[1]) {
            continue;
        }
        try {
            function();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            return 1;
        }
    }
    if (passed == 0) {
        std::cerr << "No matching test case\n";
        return 1;
    }
    return 0;
}

} // namespace test
