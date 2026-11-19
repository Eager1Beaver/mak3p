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

} // namespace audio_converter::gui
