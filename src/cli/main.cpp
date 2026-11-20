#include "app/batch.hpp"
#include "app/identity.hpp"
#include "app/import.hpp"
#include "audio/process.hpp"

#include <charconv>
#include <csignal>
#include <thread>
#include <iostream>
#include <string_view>

namespace {

using namespace audio_converter;

fs::path input_path(const std::string& text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

void print(const std::string& key, std::map<std::string, std::string> arguments = {}) {
    std::cout << message({key, std::move(arguments), {}}) << '\n';
}

void print_detail(const Diagnostic& diagnostic, bool error = false) {
    auto& stream = error ? std::cerr : std::cout;
    stream << message(diagnostic) << '\n';
    if (!diagnostic.technical_detail.empty()) stream << diagnostic.technical_detail << '\n';
}

int run(const std::vector<std::string>& arguments) {
    try {
        if (arguments.size() == 2 && arguments[1] == "--help") {
            print("cli_usage");
            return 0;
        }
        if (arguments.size() == 2 && arguments[1] == "--version") {
            print("cli_version", {{"app", application_name}, {"version", application_version}});
            return 0;
        }
        if (arguments.size() < 3 || (arguments[1] != "inspect" &&
            arguments[1] != "convert" && arguments[1] != "batch")) {
            throw AudioError({"cli_invalid_arguments", {}, {}});
        }
        const auto app_directory = executable_directory();
        SettingsStore store(app_directory);
        SettingsResult loaded;
        std::optional<fs::path> config;

    }

}

}
