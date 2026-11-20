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

}
