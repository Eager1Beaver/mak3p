#include "app/file_io.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <random>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace audio_converter::detail {

struct ExclusiveFile::Impl {
    fs::path path;
    bool retained = false;

int handle = -1;
    struct stat identity{};
    bool owned() const {
        struct stat current{};
        return ::lstat(path.c_str(), &current) == 0 && S_ISREG(current.st_mode) &&
               current.st_dev == identity.st_dev && current.st_ino == identity.st_ino;
    }
    void close() noexcept {
        if (handle >= 0) ::close(handle);
        handle = -1;
    }


~Impl() {
        if (!retained && owned()) {
            std::error_code ignored;
            fs::remove(path, ignored);
        }
        close();
    }
};


ExclusiveFile::ExclusiveFile(fs::path path) : impl_(std::make_unique<Impl>()) {
    const auto absolute = fs::absolute(path);
    if (absolute.filename().empty() || absolute.filename() == "." || absolute.filename() == "..") {
        throw std::invalid_argument("A file name is required");
    }
    impl_->path = normalize_directory(absolute.parent_path()) / absolute.filename();

    impl_->handle = ::open(impl_->path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (impl_->handle < 0 || ::fstat(impl_->handle, &impl_->identity) != 0) {
        throw fs::filesystem_error("Cannot create file exclusively", 
                                    path,
                                    std::error_code(errno, std::generic_category()));
    }

}

ExclusiveFile::~ExclusiveFile() = default;
const fs::path& ExclusiveFile::path() const noexcept { return impl_->path; }
void ExclusiveFile::retain() noexcept { impl_->retained = true; }

void ExclusiveFile::write(std::string_view bytes) {
    while (!bytes.empty()) {

        const auto written = ::write(impl_->handle, bytes.data(), bytes.size());
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) throw fs::filesystem_error("Cannot write file", 
                                                     impl_->path,
                                                     std::error_code(errno, std::generic_category()));
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
}

void ExclusiveFile::replace(const fs::path& destination) {
    if (!impl_->owned()) throw std::runtime_error("The temporary settings file was replaced");

    if (::fsync(impl_->handle) != 0) throw std::system_error(errno, std::generic_category());
    fs::rename(impl_->path, destination);
    impl_->retained = true;
}


std::unique_ptr<ExclusiveFile> temporary_file(const fs::path& directory) {
    static std::atomic<unsigned long long> sequence{0};
    static const auto seed = std::random_device{}();
    for (int attempt = 0; attempt < 128; ++attempt) {
        const auto name = ".audio-settings-" + std::to_string(seed) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(sequence.fetch_add(1)) + ".tmp";
        try { return std::make_unique<ExclusiveFile>(directory / name); }
        catch (const fs::filesystem_error& error) {
            if (error.code() != std::errc::file_exists) throw;
        }
    }
    throw std::runtime_error("Cannot allocate a temporary settings file");
}

bool writable_directory(const fs::path& directory) {
    try { const auto probe = temporary_file(directory); return true; }
    catch (const std::exception&) { return false; }
}


bool writable_existing_file(const fs::path& path) {
    const auto handle = ::open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (handle < 0) return false;
    struct stat info{};
    const bool regular = ::fstat(handle, &info) == 0 && S_ISREG(info.st_mode);
    ::close(handle);
    return regular;
}


std::string safe_log_text(std::string_view text, std::size_t limit) {
    std::string result;
    const auto size = std::min(text.size(), limit);
    for (std::size_t index = 0; index < size;) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first < 0x20 || first == 0x7f) {
            constexpr char hex[] = "0123456789abcdef";
            result += "\\x";
            result += hex[first >> 4]; result += hex[first & 15]; ++index;
            continue;
        }
        if (first < 0x80) { result += text[index++]; continue; }
        const std::size_t count = first >= 0xc2 && first <= 0xdf ? 2 :
                                  first >= 0xe0 && first <= 0xef ? 3 :
                                  first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        bool valid = count != 0 && index + count <= size;
        for (std::size_t next = 1; valid && next < count; ++next) {
            const auto byte = static_cast<unsigned char>(text[index + next]);
            valid = byte >= 0x80 && byte <= 0xbf;
        }
        if (valid) {
            const auto second = static_cast<unsigned char>(text[index + 1]);
            valid = !(first == 0xe0 && second < 0xa0) && !(first == 0xed && second >= 0xa0) &&
                    !(first == 0xf0 && second < 0x90) && !(first == 0xf4 && second >= 0x90);
        }
        if (valid) { result.append(text.substr(index, count)); index += count; }
        else { result += "\xef\xbf\xbd"; ++index; }
    }
    if (text.size() > size) result += " [truncated]";
    return result;
}

} // namespace audio_converter::detail
