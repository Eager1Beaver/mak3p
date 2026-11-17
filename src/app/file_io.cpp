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



} // namespace audio_converter::detail
