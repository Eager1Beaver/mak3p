#pragma once

#include <map>
#include <stdexcept>
#include <string>

namespace audio_converter {

struct Diagnostic {
    std::string key;
    std::map<std::string, std::string> arguments;
    std::string technical_detail;
};

[[nodiscard]] std::string message(const Diagnostic& diagnostic);

class AudioError : public std::runtime_error {
public:
    explicit AudioError(Diagnostic diagnostic);
    [[nodiscard]] const Diagnostic& diagnostic() const noexcept { return diagnostic_; }
private:
    Diagnostic diagnostic_;
};

} // namespace audio_converter
