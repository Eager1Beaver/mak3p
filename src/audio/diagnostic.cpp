#include "audio/diagnostic.hpp"
#include "audio/catalog.hpp"

#include <nlohmann/json.hpp>
#include <utility>

namespace audio_converter {

std::string message(const Diagnostic& diagnostic) {
    if (diagnostic.key.empty()) return {};
    static const auto catalog = nlohmann::json::parse(default_catalog);
    if (!catalog.contains(diagnostic.key)) {
        throw std::out_of_range("Unknown message key: " + diagnostic.key);
    }
    const auto format = catalog.at(diagnostic.key).get<std::string>();
    std::string result;
    std::size_t position = 0;
    while (position < format.size()) {
        const auto opening = format.find('{', position);
        if (opening == std::string::npos) {
            result += format.substr(position);
            break;
        }
        result += format.substr(position, opening - position);
        const auto closing = format.find('}', opening);
        if (closing == std::string::npos) throw std::logic_error("Invalid message placeholder");
        result += diagnostic.arguments.at(format.substr(opening + 1, closing - opening - 1));
        position = closing + 1;
    }
    return result;
}

AudioError::AudioError(Diagnostic diagnostic)
    : std::runtime_error(message(diagnostic)), diagnostic_(std::move(diagnostic)) {}

} // namespace audio_converter
