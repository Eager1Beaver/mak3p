#pragma once

#include "core/path_utils.hpp"

#include <chrono>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace audio_converter {

struct ProcessRequest {
    fs::path executable;
    std::vector<std::string> arguments;
    std::chrono::milliseconds timeout{0};
    std::size_t output_limit = 4 * 1024 * 1024;
    std::function<void(std::string_view)> on_stdout{};
};

struct ProcessResult {
    int exit_code = -1;
    std::string standard_output;
    std::string standard_error;
    bool stdout_truncated = false;
    bool stderr_truncated = false;
    bool cancelled = false;
    bool timed_out = false;
};

[[nodiscard]] fs::path resolve_executable(const fs::path& executable);
[[nodiscard]] std::string path_utf8(const fs::path& path);

// Runs an argument vector without a shell and drains both output streams.
[[nodiscard]] ProcessResult run_process(const ProcessRequest& request,
                                       std::stop_token stop = {});

} // namespace audio_converter
