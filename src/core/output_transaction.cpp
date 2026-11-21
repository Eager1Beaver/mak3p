#include "core/output_transaction.hpp"

#include <cerrno>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#endif

namespace audio_converter {

namespace {

std::string random_token() {
    constexpr char digits[] = "0123456789abcdef";
    std::random_device random;
    std::string token;
    token.reserve(32);
    for (int block = 0; block < 4; ++block) {
        auto value = static_cast<std::uint32_t>(random());
        for (int digit = 0; digit < 8; ++digit) {
            token.push_back(digits[value & 0xf]);
            value >>= 4;
        }
    }
    return token;
}

enum class Ownership { Missing, Owned, Changed };
enum class Publication { Collision, Moved, Linked };

} // namespace

struct OutputTransaction::Impl {
    OutputPathPlanner& planner;
    OutputRequest request;
    fs::path final;
    fs::path temporary;
    bool published = false;
    bool owns_temporary = false;
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION identity{};
#else
    int handle = -1;
    struct stat identity{};
#endif

    Impl(OutputPathPlanner& owner, OutputRequest input)
        : planner(owner), request(std::move(input)) {}

    ~Impl() {
        try {
            discard();
        } catch (...) {
        }
        close_handle();
    }

    void close_handle() noexcept {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
#else
        if (handle >= 0) {
            ::close(handle);
            handle = -1;
        }
#endif
    }

    void initialize() {
        final = planner.reserve(request);
        request.source = fs::absolute(request.source);
        request.destination_directory = final.parent_path();
        for (int attempt = 0; attempt < 128; ++attempt) {
            temporary = final.parent_path() /
                (".audio-converter-" + random_token() + ".partial.mp3");
#ifdef _WIN32
            const auto native = windows_api_path(temporary);
            // Retain identity without imposing read/write sharing on media libraries.
            handle = CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                const auto error = GetLastError();
                if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
                    continue;
                }
                throw fs::filesystem_error("Cannot create temporary output", temporary,
                    std::error_code(static_cast<int>(error), std::system_category()));
            }
            if (!GetFileInformationByHandle(handle, &identity)) {
                throw fs::filesystem_error("Cannot identify temporary output", temporary,
                    std::error_code(static_cast<int>(GetLastError()), std::system_category()));
            }
#else
            handle = ::open(temporary.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
            if (handle < 0) {
                const int error = errno;
                if (error == EEXIST) {
                    continue;
                }
                throw fs::filesystem_error("Cannot create temporary output", temporary,
                    std::error_code(error, std::generic_category()));
            }
            if (::fstat(handle, &identity) != 0) {
                throw fs::filesystem_error("Cannot identify temporary output", temporary,
                    std::error_code(errno, std::generic_category()));
            }
#endif
            owns_temporary = true;
            return;
        }
        throw std::runtime_error("Cannot allocate a unique temporary output");
    }

    Ownership ownership(bool require_content) const {
#ifdef _WIN32
        const auto native = windows_api_path(temporary);
        const HANDLE current = CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (current == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                return Ownership::Missing;
            }
            throw fs::filesystem_error("Cannot inspect temporary output", temporary,
                std::error_code(static_cast<int>(error), std::system_category()));
        }
        BY_HANDLE_FILE_INFORMATION info{};
        const bool obtained = GetFileInformationByHandle(current, &info) != 0;
        const auto error = GetLastError();
        CloseHandle(current);
        if (!obtained) {
            throw fs::filesystem_error("Cannot inspect temporary output", temporary,
                std::error_code(static_cast<int>(error), std::system_category()));
        }
        if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            info.dwVolumeSerialNumber != identity.dwVolumeSerialNumber ||
            info.nFileIndexHigh != identity.nFileIndexHigh ||
            info.nFileIndexLow != identity.nFileIndexLow) {
            return Ownership::Changed;
        }
        const bool nonempty = info.nFileSizeHigh != 0 || info.nFileSizeLow != 0;
#else
        struct stat info{};
        if (::lstat(temporary.c_str(), &info) != 0) {
            const int error = errno;
            if (error == ENOENT || error == ENOTDIR) {
                return Ownership::Missing;
            }
            throw fs::filesystem_error("Cannot inspect temporary output", temporary,
                std::error_code(error, std::generic_category()));
        }
        if (!S_ISREG(info.st_mode) || info.st_dev != identity.st_dev ||
            info.st_ino != identity.st_ino) {
            return Ownership::Changed;
        }
        const bool nonempty = info.st_size > 0;
#endif
        if (require_content && !nonempty) {
            throw std::runtime_error("Temporary output is empty");
        }
        return Ownership::Owned;
    }

    Publication publish() const {
#ifdef _WIN32
        const auto source = windows_api_path(temporary);
        const auto destination = windows_api_path(final);
        if (MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
            return Publication::Moved;
        }
        const auto error = GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
            return Publication::Collision;
        }
        if (error == ERROR_ACCESS_DENIED) {
            std::error_code status_error;
            const auto status = fs::symlink_status(fs::path(destination), status_error);
            if (!status_error && status.type() != fs::file_type::not_found) {
                return Publication::Collision;
            }
        }
        throw fs::filesystem_error("Cannot commit output", temporary, final,
            std::error_code(static_cast<int>(error), std::system_category()));
#elif defined(__linux__)
#ifdef SYS_renameat2
        if (::syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(),
                      AT_FDCWD, final.c_str(), RENAME_NOREPLACE) == 0) {
            return Publication::Moved;
        }
        const int error = errno;
#else
        const int error = ENOSYS;
#endif
        if (error == EEXIST) {
            return Publication::Collision;
        }
        throw fs::filesystem_error(
            "Cannot commit output without replacement", 
            temporary, 
            final,
            std::error_code(error, std::generic_category())
        );
#else
        if (::link(temporary.c_str(), final.c_str()) == 0) {
            return Publication::Linked;
        }
        const int error = errno;
        if (error == EEXIST) {
            return Publication::Collision;
        }
        throw fs::filesystem_error("Cannot publish output without replacement", temporary, final,
            std::error_code(error, std::generic_category()));
#endif
    }

    void discard() {
        if (!owns_temporary) {
            return;
        }
        const auto state = ownership(false);
        if (state == Ownership::Changed) {
            throw std::runtime_error("Temporary output no longer belongs to this job");
        }
        if (state == Ownership::Owned) {
#ifdef _WIN32
            const auto native = windows_api_path(temporary);
            if (!DeleteFileW(native.c_str())) {
                throw fs::filesystem_error(
                    "Cannot remove temporary output", 
                    temporary,
                    std::error_code(static_cast<int>(GetLastError()), 
                    std::system_category())
                );
            }
#else
            if (::unlink(temporary.c_str()) != 0) {
                throw fs::filesystem_error(
                    "Cannot remove temporary output", 
                    temporary,
                    std::error_code(errno, std::generic_category())
                );
            }
#endif
        }
        owns_temporary = false;
        close_handle();
    }
};

OutputTransaction::OutputTransaction(OutputPathPlanner& planner, OutputRequest request)
    : impl_(std::make_unique<Impl>(planner, std::move(request))) {
    impl_->initialize();
}

OutputTransaction::~OutputTransaction() = default;

const fs::path& OutputTransaction::temporary_path() const noexcept {
    return impl_->temporary;
}

const fs::path& OutputTransaction::final_path() const noexcept {
    return impl_->final;
}

bool OutputTransaction::committed() const noexcept {
    return impl_->published;
}

bool OutputTransaction::has_temporary_file() const noexcept {
    return impl_->owns_temporary;
}

void OutputTransaction::validate_temporary() const {
    if (!impl_->owns_temporary || impl_->ownership(false) != Ownership::Owned) {
        throw std::runtime_error("Temporary output is missing or no longer owned");
    }
}

fs::path OutputTransaction::commit() {
    if (impl_->published) {
        return impl_->final;
    }
    if (!impl_->owns_temporary) {
        throw std::logic_error("The temporary output has been discarded");
    }
    for (int attempt = 0; attempt < 1024; ++attempt) {
        if (impl_->ownership(true) != Ownership::Owned) {
            throw std::runtime_error("Temporary output is missing or no longer owned");
        }
        const auto result = impl_->publish();
        if (result == Publication::Collision) {
            impl_->final = impl_->planner.reserve(impl_->request);
            continue;
        }
        impl_->published = true;
        if (result == Publication::Moved) {
            impl_->owns_temporary = false;
            impl_->close_handle();
        } else {
            try {
                impl_->discard();
            } catch (...) {
                // Publication succeeded; retain ownership for a cleanup retry.
            }
        }
        return impl_->final;
    }
    throw std::runtime_error("Output destination kept changing during commit");
}

void OutputTransaction::discard() {
    impl_->discard();
}

} // namespace audio_converter
