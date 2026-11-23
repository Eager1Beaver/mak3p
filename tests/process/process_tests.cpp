#include "audio/diagnostic.hpp"
#include "audio/process.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#endif

using namespace audio_converter;
using namespace std::chrono_literals;
using test::require;
using test::throws;
using test::TempDirectory;

namespace {

fs::path helper;

void arguments() {
    ProcessRequest request{helper, {"arguments", "", "space inside", "quote\"inside", "trailing\\",
                                   "semi;colon", "$(touch should-not-exist)", "amp&pipe|bracket(",
                                   "日本語 🎵", "line\nbreak"}};
    std::string expected;
    for (std::size_t index = 1; index < request.arguments.size(); ++index) {
        expected += std::to_string(request.arguments[index].size()) + ":" +
                    request.arguments[index] + "\n";
    }
    const auto result = run_process(request);
    require(result.exit_code == 0 && result.standard_output == expected,
            "An argument was split, escaped incorrectly, or interpreted by a shell");
}

void unicode_executable() {
    TempDirectory tmp;
    const auto folder = tmp.path / fs::path(u8"tools 日本語");
    fs::create_directory(folder);
    auto target = folder / "helper spaced";
#ifdef _WIN32
    target += ".exe";
#endif
    fs::copy_file(helper, target);
    const auto result = run_process({target, {"arguments", "literal"}});
    require(result.exit_code == 0 && result.standard_output == "7:literal\n",
            "Unicode or spaced executable path failed");
}

void pipe_limits() {
    ProcessRequest request{helper, {"flood"}, 10s};
    request.output_limit = 4096;
    std::size_t observed = 0;
    request.on_stdout = [&](std::string_view bytes) { observed += bytes.size(); };
    const auto result = run_process(request);
    require(result.exit_code == 0 && result.standard_output == std::string(4096, 'o') &&
            result.standard_error == std::string(4096, 'e') &&
            result.stdout_truncated && result.stderr_truncated && observed == 128 * 8192,
            "Both pipes must drain completely while captured output remains bounded");
    request.output_limit = 0;
    observed = 0;
    const auto zero = run_process(request);
    require(zero.exit_code == 0 && zero.standard_output.empty() && zero.standard_error.empty() &&
            observed == 128 * 8192, "Zero capture limit must still drain pipes and invoke callbacks");
}

void exit_status() {
    require(run_process({helper, {"exit"}}).exit_code == 7, "Child exit status was lost");
}

void missing_executable() {
    throws<fs::filesystem_error>([] { (void)run_process({"missing-audio-tool-93ad01", {}}); },
                                 "A missing executable was accepted");
}

void timeout() {
    const auto start = std::chrono::steady_clock::now();
    const auto result = run_process({helper, {"ignore"}, 60ms});
    require(result.timed_out && !result.cancelled && result.exit_code != 0 &&
            std::chrono::steady_clock::now() - start < 3s,
            "A timeout must terminate and reap even an uncooperative process");
}

void cancellation() {
    std::stop_source stop;
    std::jthread cancel([&] {
        std::this_thread::sleep_for(60ms);
        stop.request_stop();
    });
    const auto result = run_process({helper, {"sleep"}, 5s}, stop.get_token());
    require(result.cancelled && !result.timed_out && result.exit_code != 0,
            "Stop token did not terminate the child");
    const auto before = run_process({helper, {"flood"}}, stop.get_token());
    require(before.cancelled && before.standard_output.empty(),
            "A pre-cancelled request must not launch its executable");
}

void callback_cleanup() {
    ProcessRequest request{helper, {"sleep"}, 5s};
    long child = -1;
    request.on_stdout = [&](std::string_view bytes) {
        child = std::stol(std::string(bytes));
        throw std::runtime_error("observer failed");
    };
    throws<std::runtime_error>([&] { (void)run_process(request); },
                               "Callback failure was not propagated");
#ifndef _WIN32
    errno = 0;
    require(child > 0 && ::kill(static_cast<pid_t>(child), 0) != 0 && errno == ESRCH,
            "Callback failure left its child running");
#else
    require(child > 0, "The callback did not observe the running child");
#endif
}

void invalid_arguments() {
    ProcessRequest request{helper, {"arguments", std::string("x\0y", 3)}};
    throws<std::invalid_argument>([&] { (void)run_process(request); }, "Embedded NUL accepted");
    request.arguments.clear();
    request.timeout = -1ms;
    throws<std::invalid_argument>([&] { (void)run_process(request); }, "Negative timeout accepted");
}

void descendant_cleanup() {
    TempDirectory tmp;
    const auto marker = tmp.path / "survived";
    std::stop_source stop;
    ProcessRequest request{helper, {"descendant", path_utf8(marker)}, 5s};
    request.on_stdout = [&](std::string_view) { stop.request_stop(); };
    const auto result = run_process(request, stop.get_token());
    require(result.cancelled && !result.standard_output.empty(),
            "Descendant test did not launch and cancel the helper");
    std::this_thread::sleep_for(1s);
    require(!fs::exists(marker), "Cancellation left a descendant process alive");
}

void localization() {
    const auto filter = message({"gui_files_filter", {{"extensions", "*.{flac,ape,wv,mp3}"}}, {}});
    require(filter.find("*.{flac,ape,wv") != std::string::npos && filter.find("mp3}") != std::string::npos &&
            filter.find('\t') != std::string::npos && filter.find('\n') != std::string::npos,
            "Literal braces or chooser separators were interpreted as message placeholders");
    const auto result = message({"cli_input", {{"path", "{status} 日本語"}}, {}});
    require(result == "Input: {status} 日本語", "Inserted text was interpreted as a placeholder");
    require(message({"same_bitrate", {}, {}}) == "Same bitrate — skipped.",
            "Unexpected catalog text: " + message({"same_bitrate", {}, {}}));
    throws<std::out_of_range>([] { (void)message({"unknown-key", {}, {}}); },
                              "Missing message key was silently accepted");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    helper = argv[2];
    return test::run(argc, argv, {
        {"arguments", arguments}, {"unicode_executable", unicode_executable},
        {"pipe_limits", pipe_limits}, {"exit_status", exit_status},
        {"missing_executable", missing_executable}, {"timeout", timeout},
        {"cancellation", cancellation}, {"callback_cleanup", callback_cleanup},
        {"invalid_arguments", invalid_arguments}, {"descendant_cleanup", descendant_cleanup},
        {"localization", localization}
    });
}
