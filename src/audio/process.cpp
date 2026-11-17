#include "audio/process.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

namespace audio_converter {
namespace {

void capture(std::string& destination, 
             bool& truncated, 
             std::string_view bytes,
             std::size_t limit) {
    const auto count = std::min(bytes.size(), limit - destination.size());
    destination.append(bytes.data(), count);
    truncated = truncated || count != bytes.size();
}

void validate(const ProcessRequest& request) {
    if (request.timeout.count() < 0) {
        throw std::invalid_argument("Process timeout must not be negative");
    }
    for (const auto& argument : request.arguments) {
        if (argument.find('\0') != std::string::npos) {
            throw std::invalid_argument("Process argument contains an embedded NUL");
        }
    }
}

struct Descriptor {
    int value = -1;
    Descriptor() = default;
    explicit Descriptor(int input) : value(input) {}
    ~Descriptor() { close(); }
    void close() noexcept {
        if (value >= 0) ::close(value);
        value = -1;
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
};


} // namespace audio_converter

}
