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

} // namespace audio_converter::gui
