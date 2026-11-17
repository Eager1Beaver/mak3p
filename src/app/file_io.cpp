#include "app/file_io.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <random>
#include <system_error>


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


} // namespace audio_converter::detail
