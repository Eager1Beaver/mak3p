#include "gui/presentation.hpp"
#include "audio/process.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace audio_converter::gui {
std::string text(const std::string& key, std::map<std::string, std::string> arguments) {
    return message({key, std::move(arguments), {}});
}
std::string duration(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) return text("gui_na");
    const auto total = static_cast<unsigned long long>(std::min(seconds, 359999999.0));
    std::ostringstream out;
    if (total >= 3600) out << total / 3600 << ':';
    out << std::setfill('0') << std::setw(2) << (total / 60) % 60 << ':' << std::setw(2) << total % 60;
    return out.str();
}
std::vector<unsigned> worker_options(unsigned detected, unsigned saved) {
    const auto ceiling = detected ? std::min(detected, maximum_worker_override) : maximum_worker_override;
    std::vector<unsigned> result{0};
    for (unsigned value : {1U, 2U, 4U, 6U, 8U, 12U, 16U, 24U, 32U, 64U})
        if (value <= ceiling) result.push_back(value);
    result.push_back(ceiling);
    if (saved && saved <= ceiling) result.push_back(saved);
    std::sort(result.begin(), result.end()); result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
std::string phase_text(GuiPhase phase) {
    switch (phase) {
    case GuiPhase::Initializing: return text("gui_initializing");
    case GuiPhase::Importing: return text("gui_importing");
    case GuiPhase::Checking: return text("gui_checking");
    case GuiPhase::Running: return text("gui_running");
    case GuiPhase::Cancelling: return text("gui_cancelling");
    case GuiPhase::EngineMissing: return text("engine_missing");
    case GuiPhase::Closing: case GuiPhase::Closed: return text("gui_closing");
    case GuiPhase::Idle: return {};
    }
    return {};
}
RowText present_row(const JobSnapshot& row, std::size_t position, const AppSettings& settings) {
    RowText view{row.job.id(), {}, {}, {}, {}, {}, row.job.progress(), row.progress_visible, false, false};
    std::ostringstream number; number << std::setfill('0') << std::setw(2) << position;
    view.name = number.str() + " · " + path_utf8(row.job.source().path.filename());
    const auto na = text("gui_na");
    auto decimal = [](double value, int precision) { std::ostringstream out; out << std::fixed << std::setprecision(precision) << value; return out.str(); };
    std::string codec = na, rate = na, bitrate = na, channels = na, size = na, length = na;
    if (row.media) {
        codec = row.media->audio.codec;
        std::transform(codec.begin(), codec.end(), codec.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (row.media->audio.sample_rate) rate = text("gui_khz", {{"value", decimal(row.media->audio.sample_rate / 1000.0, row.media->audio.sample_rate % 1000 ? 1 : 0)}});
        if (row.media->bitrate_bps) bitrate = text("gui_kbps", {{"value", std::to_string(*row.media->bitrate_bps / 1000)}});
        if (row.media->audio.channels == 1) channels = text("gui_mono");
        else if (row.media->audio.channels == 2) channels = text("gui_stereo");
        else if (row.media->audio.channels > 0) channels = text("gui_channel_count", {{"value", std::to_string(row.media->audio.channels)}});
        const auto time = row.media->duration_seconds ? row.media->duration_seconds : row.media->audio.duration_seconds;
        if (time) length = duration(*time);
    }
    if (row.source_size) size = text("gui_mb", {{"value", decimal(*row.source_size / 1000000.0, 2)}});
    view.properties = codec + " · " + rate + " · " + bitrate + " · " + channels + " · " + size + " · " + length;
    std::string key;
    switch (row.job.state()) {
    case JobState::Checking: key = "gui_status_checking"; break;
    case JobState::Ready: key = "gui_status_ready"; break;
    case JobState::Converting: key = "gui_status_converting"; break;
    case JobState::Completed: key = "gui_status_completed"; break;
    case JobState::Failed: key = "gui_status_failed"; view.failed = true; break;
    case JobState::Cancelled: key = "gui_status_cancelled"; break;
    default: key = "gui_status_skipped"; view.warning = true; break;
    }

}

} // namespace audio_converter::gui
