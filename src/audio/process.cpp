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

void posix_check(int error, const char* context) {
    if (error != 0) throw std::system_error(error, std::generic_category(), context);
}

struct Pipe {
    Descriptor read;
    Descriptor write;
    Pipe() {
        int descriptors[2];
#ifdef __linux__
        if (::pipe2(descriptors, O_CLOEXEC) != 0) {
#else
        if (::pipe(descriptors) != 0) {
#endif
            posix_check(errno, "Cannot create pipe");
        }
        read.value = descriptors[0];
        write.value = descriptors[1];
        for (auto* descriptor : {&read, &write}) {
            if (descriptor->value < 3) {
                const int copy = ::fcntl(descriptor->value, F_DUPFD_CLOEXEC, 3);
                if (copy < 0) posix_check(errno, "Cannot duplicate pipe");
                descriptor->close();
                descriptor->value = copy;
            }
            if (::fcntl(descriptor->value, F_SETFD, FD_CLOEXEC) != 0) {
                posix_check(errno, "Cannot protect pipe inheritance");
            }
        }
    }
};

} // namespace audio_converter

}
