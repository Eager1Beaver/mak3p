#include "audio/process.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

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

#ifdef _WIN32

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE input) : value(input) {}
    ~Handle() { close(); }
    void close() noexcept {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = INVALID_HANDLE_VALUE;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

[[noreturn]] void win_error(const char* context) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), context);
}

std::wstring wide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                          static_cast<int>(utf8.size()), nullptr, 0);
    if (count == 0) win_error("Invalid UTF-8 process argument");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                            static_cast<int>(utf8.size()), result.data(), count)) {
        win_error("Cannot convert process argument");
    }
    return result;
}

std::wstring quote(const std::wstring& argument) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        result.append(slashes * (character == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (character == L'"') result += L'\\';
        result += character;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}

struct Pipe {
    Handle read;
    Handle write;
    Pipe() {
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        if (!CreatePipe(&read.value, &write.value, &security, 0)) win_error("Cannot create pipe");
        if (!SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0)) {
            win_error("Cannot restrict pipe inheritance");
        }
    }
};

struct RunningProcess {
    HANDLE job;
    HANDLE process;
    ~RunningProcess() {
        if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
            TerminateJobObject(job, 1);
            TerminateProcess(process, 1);
            WaitForSingleObject(process, INFINITE);
        }
    }
};

struct Attributes {
    std::vector<unsigned char> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
    explicit Attributes(std::array<HANDLE, 3>& handles) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        storage.resize(size);
        auto* candidate = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(candidate, 1, 0, &size)) {
            win_error("Cannot initialize process attributes");
        }
        list = candidate;
        if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       handles.data(), sizeof(handles), nullptr, nullptr)) {
            const auto error = GetLastError();
            DeleteProcThreadAttributeList(list);
            list = nullptr;
            SetLastError(error);
            win_error("Cannot restrict process inheritance");
        }
    }
    ~Attributes() { if (list) DeleteProcThreadAttributeList(list); }
};

#else

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

struct SpawnActions {
    posix_spawn_file_actions_t value{};
    SpawnActions() { posix_check(posix_spawn_file_actions_init(&value), "Cannot initialize spawn"); }
    ~SpawnActions() { posix_spawn_file_actions_destroy(&value); }
};

struct SpawnAttributes {
    posix_spawnattr_t value{};
    SpawnAttributes() { posix_check(posix_spawnattr_init(&value), "Cannot initialize spawn"); }
    ~SpawnAttributes() { posix_spawnattr_destroy(&value); }
};

struct Child {
    pid_t pid = -1;
    bool reaped = false;
    ~Child() {
        if (pid > 0 && !reaped) {
            ::kill(-pid, SIGKILL);
            while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
        }
    }
};

#endif

} // Pipe

std::string path_utf8(const fs::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

fs::path resolve_executable(const fs::path& executable) {
    if (executable.empty() ||
        executable.native().find(fs::path::value_type{}) != fs::path::string_type::npos) {
        throw std::invalid_argument("Executable path is invalid");
    }
#ifdef _WIN32
    if (executable.has_parent_path()) {
        const auto path = fs::canonical(executable);
        if (!fs::is_regular_file(path)) throw std::invalid_argument("Executable is not a file");
        return path;
    }
    std::wstring buffer(32768, L'\0');
    const DWORD length = SearchPathW(nullptr, 
                                     executable.c_str(), 
                                     L".exe",
                                     static_cast<DWORD>(buffer.size()), 
                                     buffer.data(), 
                                     nullptr);
    if (length == 0 || length >= buffer.size()) win_error("Cannot find executable");
    buffer.resize(length);
    return fs::canonical(buffer);
#else
    if (executable.has_parent_path()) {
        const auto path = fs::canonical(executable);
        if (!fs::is_regular_file(path) || ::access(path.c_str(), X_OK) != 0) {
            throw fs::filesystem_error("Executable is not runnable", path,
                                       std::make_error_code(std::errc::permission_denied));
        }
        return path;
    }
    const char* environment = std::getenv("PATH");
    const std::string search = environment ? environment : "/usr/bin:/bin";
    std::size_t offset = 0;
    while (offset <= search.size()) {
        const auto separator = search.find(':', offset);
        const auto component = search.substr(offset, separator - offset);
        const auto path = fs::path(component.empty() ? "." : component) / executable;
        std::error_code error;
        if (fs::is_regular_file(path, error) && ::access(path.c_str(), X_OK) == 0) {
            return fs::canonical(path);
        }
        if (separator == std::string::npos) break;
        offset = separator + 1;
    }
    throw fs::filesystem_error("Cannot find executable", executable,
                               std::make_error_code(std::errc::no_such_file_or_directory));
#endif
}    

ProcessResult run_process(const ProcessRequest& request, std::stop_token stop) {
    validate(request);
    ProcessResult result;
    if (stop.stop_requested()) {
        result.cancelled = true;
        return result;
    }
    const auto executable = resolve_executable(request.executable);
    Pipe output;
    Pipe error;
    const auto start = std::chrono::steady_clock::now();

Descriptor input(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    if (input.value < 0) posix_check(errno, "Cannot redirect standard input");
    SpawnActions actions;
    SpawnAttributes attributes;
    for (const auto& pair : {std::pair{input.value, STDIN_FILENO},
                            std::pair{output.write.value, STDOUT_FILENO},
                            std::pair{error.write.value, STDERR_FILENO}}) {
        posix_check(posix_spawn_file_actions_adddup2(&actions.value, pair.first, pair.second),
                    "Cannot redirect process stream");
    }
    for (int descriptor : {output.read.value, output.write.value, error.read.value,
                            error.write.value, input.value}) {
        if (descriptor > STDERR_FILENO) {
            posix_check(posix_spawn_file_actions_addclose(&actions.value, descriptor),
                        "Cannot close inherited descriptor");
        }
    }
    posix_check(posix_spawnattr_setpgroup(&attributes.value, 0), "Cannot set process group");
    posix_check(posix_spawnattr_setflags(&attributes.value, POSIX_SPAWN_SETPGROUP),
                "Cannot set spawn flags");
    std::vector<std::string> storage{executable.native()};
    storage.insert(storage.end(), request.arguments.begin(), request.arguments.end());
    std::vector<char*> arguments;
    for (auto& argument : storage) arguments.push_back(argument.data());
    arguments.push_back(nullptr);
    Child child;
    posix_check(posix_spawn(&child.pid, executable.c_str(), &actions.value,
                            &attributes.value, arguments.data(), environ), "Cannot start process");
    input.close();
    output.write.close();
    error.write.close();
    for (int descriptor : {output.read.value, error.read.value}) {
        const int flags = ::fcntl(descriptor, F_GETFL);
        if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
            posix_check(errno, "Cannot configure process output");
        }
    }
    bool stopping = false;
    auto stopped = start;
    auto exited = start;
    const auto drain = [&](Descriptor& pipe, bool standard_output) {
        std::array<char, 16384> buffer;
        for (int chunk = 0; chunk < 64 && pipe.value >= 0; ++chunk) {
            const auto count = ::read(pipe.value, buffer.data(), buffer.size());
            if (count > 0) {
                const std::string_view bytes(buffer.data(), static_cast<std::size_t>(count));
                if (standard_output) {
                    capture(result.standard_output, result.stdout_truncated, bytes, request.output_limit);
                    if (request.on_stdout) request.on_stdout(bytes);
                } else {
                    capture(result.standard_error, result.stderr_truncated, bytes, request.output_limit);
                }
            } else if (count == 0) {
                pipe.close();
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else if (errno != EINTR) {
                posix_check(errno, "Cannot read process output");
            }
        }
    };
    while (!child.reaped || output.read.value >= 0 || error.read.value >= 0) {
        const auto now = std::chrono::steady_clock::now();
        if (!child.reaped && !stopping && (stop.stop_requested() ||
            (request.timeout.count() > 0 && now - start >= request.timeout))) {
            result.cancelled = stop.stop_requested();
            result.timed_out = !result.cancelled;
            stopping = true;
            stopped = now;
            ::kill(-child.pid, SIGTERM);
        }
        if (!child.reaped && stopping && now - stopped >= std::chrono::milliseconds(500)) {
            ::kill(-child.pid, SIGKILL);
        }
        drain(output.read, true);
        drain(error.read, false);
        if (!child.reaped) {
            int status = 0;
            const auto waited = ::waitpid(child.pid, &status, WNOHANG);
            if (waited == child.pid) {
                child.reaped = true;
                exited = now;
                result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                ::kill(-child.pid, SIGKILL);
            } else if (waited < 0 && errno != EINTR) {
                posix_check(errno, "Cannot wait for process");
            }
        } else if (now - exited > std::chrono::milliseconds(250)) {
            output.read.close();
            error.read.close();
        }
        if (!child.reaped || output.read.value >= 0 || error.read.value >= 0) {
            pollfd descriptors[2]{{output.read.value, POLLIN, 0}, {error.read.value, POLLIN, 0}};
            if (::poll(descriptors, 2, 20) < 0 && errno != EINTR) {
                posix_check(errno, "Cannot wait for process output");
            }
        }
    }
#endif
    return result;    

} // namespace audio_converter

}
