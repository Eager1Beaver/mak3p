#pragma once

#include "core/path_utils.hpp"

#include <memory>
#include <string_view>

namespace audio_converter::detail {

// Exclusively creates a file and removes only that owned entry unless retained.
class ExclusiveFile {
public:
    explicit ExclusiveFile(fs::path path);
    ~ExclusiveFile();
    ExclusiveFile(const ExclusiveFile&) = delete;
    ExclusiveFile& operator=(const ExclusiveFile&) = delete;
    void write(std::string_view bytes);
    void replace(const fs::path& destination);
    void retain() noexcept;
    [[nodiscard]] const fs::path& path() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::unique_ptr<ExclusiveFile> temporary_file(const fs::path& directory);
[[nodiscard]] bool writable_directory(const fs::path& directory);
[[nodiscard]] bool writable_existing_file(const fs::path& path);
[[nodiscard]] std::string safe_log_text(std::string_view text, std::size_t limit = 8192);

} // namespace audio_converter::detail
