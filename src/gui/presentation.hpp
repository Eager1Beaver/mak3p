#pragma once

#include "app/gui_controller.hpp"

namespace audio_converter::gui {
[[nodiscard]] std::string text(const std::string& key, std::map<std::string, std::string> arguments = {});
[[nodiscard]] std::string duration(double seconds);
[[nodiscard]] std::vector<unsigned> worker_options(unsigned detected, unsigned saved);
struct RowText {
    JobId id;
    std::string name, properties, status, tooltip, remaining;
    std::optional<double> progress;
    bool progress_visible = false;
    bool warning = false;
    bool failed = false;
};
[[nodiscard]] RowText present_row(const JobSnapshot& row, std::size_t position, const AppSettings& settings);
[[nodiscard]] std::string phase_text(GuiPhase phase);
} // namespace audio_converter::gui
