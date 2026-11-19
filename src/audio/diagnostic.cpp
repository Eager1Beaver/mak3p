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
        const char current = format[position];
        if (current == '{' && position + 1 < format.size() && format[position + 1] == '{') {
            result += '{'; position += 2;
        } else if (current == '}' && position + 1 < format.size() && format[position + 1] == '}') {
            result += '}'; position += 2;
        } else if (current == '{') {
            const auto closing = format.find('}', position + 1);
            if (closing == std::string::npos) throw std::logic_error("Invalid message placeholder");
            result += diagnostic.arguments.at(format.substr(position + 1, closing - position - 1));
            position = closing + 1;
        } else if (current == '}') {
            throw std::logic_error("Unescaped closing brace in message");
        } else {
            result += current; ++position;
        }
    }
    return result;
}

AudioError::AudioError(Diagnostic diagnostic)
    : std::runtime_error(message(diagnostic)), diagnostic_(std::move(diagnostic)) {}

} // namespace audio_converter
