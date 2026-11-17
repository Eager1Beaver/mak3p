#pragma once

#include "app/file_io.hpp"
#include "app/settings.hpp"

namespace audio_converter {

class RunLog {
public:
    RunLog(const fs::path& application_directory, const std::string& application_version,
           const std::string& ffmpeg_version, const std::string& ffprobe_version,
           const AppSettings& settings, std::span<const ConversionJob> inputs);
    [[nodiscard]] const fs::path& path() const noexcept;
    void outcome(const ConversionJob& job, const Diagnostic& diagnostic,
                 const std::vector<Diagnostic>& warnings,
                 std::optional<int> output_bitrate_kbps = std::nullopt);
    void finish(std::size_t completed, std::size_t skipped, std::size_t failed,
                std::size_t cancelled, std::size_t excluded);
private:
    std::unique_ptr<detail::ExclusiveFile> file_;
    void line(const std::string& text);
    void diagnostic(const Diagnostic& diagnostic);
};

} // namespace audio_converter
