#include "app/run_log.hpp"
#include "app/identity.hpp"
#include "audio/process.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace audio_converter {

RunLog::RunLog(const fs::path& directory, const std::string& application_version,
               const std::string& ffmpeg_version, const std::string& ffprobe_version,
               const AppSettings& settings, std::span<const ConversionJob> inputs) {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream timestamp;
    timestamp << std::put_time(&utc, "%Y-%m-%d_%H%M%S");
    for (unsigned suffix = 0; suffix < 10000; ++suffix) {
        const auto name = std::string(application_name) + "_" + timestamp.str() +
            (suffix ? "_" + std::to_string(suffix) : "") + ".log";
        try {
            file_ = std::make_unique<detail::ExclusiveFile>(directory / name);
            break;
        } catch (const fs::filesystem_error& error) {
            if (error.code() != std::errc::file_exists) throw;
        }
    }
    if (!file_) throw std::runtime_error("Cannot allocate a unique log filename");
    file_->retain();
    line(message({"log_header", {{"timestamp", timestamp.str() + " UTC"},
        {"app", detail::safe_log_text(application_version)},
        {"ffmpeg", detail::safe_log_text(ffmpeg_version)},
        {"ffprobe", detail::safe_log_text(ffprobe_version)}}, {}}));
    std::istringstream setting_lines(serialize_settings(settings));
    for (std::string setting; std::getline(setting_lines, setting);) line(detail::safe_log_text(setting));
    for (const auto& input : inputs) {
        line(message({"log_input", {{"id", std::to_string(input.id())},
            {"path", detail::safe_log_text(path_utf8(input.source().path))}}, {}}));
    }
}

const fs::path& RunLog::path() const noexcept { return file_->path(); }
void RunLog::line(const std::string& text) { file_->write(text + "\n"); }
void RunLog::diagnostic(const Diagnostic& value) {
    line("  " + detail::safe_log_text(message(value)));
    if (!value.technical_detail.empty()) line("  " + detail::safe_log_text(value.technical_detail));
}

void RunLog::outcome(const ConversionJob& job, const Diagnostic& value,
                      const std::vector<Diagnostic>& warnings,
                      std::optional<int> output_bitrate_kbps) {
    line(message({"log_result", {{"id", std::to_string(job.id())},
        {"path", detail::safe_log_text(path_utf8(job.source().path))},
        {"status", detail::safe_log_text(message(value))}}, {}}));
    if (output_bitrate_kbps) line("  " + message({"cli_output_bitrate",
        {{"bitrate", std::to_string(*output_bitrate_kbps)}}, {}}));
    if (job.output()) line(message({"cli_output", {{"path", detail::safe_log_text(path_utf8(*job.output()))}}, {}}));
    if (!value.technical_detail.empty()) line("  " + detail::safe_log_text(value.technical_detail));
    for (const auto& warning : warnings) diagnostic(warning);
}

void RunLog::finish(std::size_t completed, std::size_t skipped, std::size_t failed,
                    std::size_t cancelled, std::size_t excluded) {
    line(message({"batch_summary", {{"completed", std::to_string(completed)},
        {"skipped", std::to_string(skipped)}, {"failed", std::to_string(failed)},
        {"cancelled", std::to_string(cancelled)}, {"excluded", std::to_string(excluded)}}, {}}));
}

} // namespace audio_converter
