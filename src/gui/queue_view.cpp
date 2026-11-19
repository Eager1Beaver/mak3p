#include "gui/queue_view.hpp"
#include "gui/constants.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_draw.H>
#include <algorithm>

namespace audio_converter::gui {
namespace {
std::string shortened(std::string value, int width) {
    if (fl_width(value.c_str()) <= width) return value;
    while (!value.empty() && fl_width((value + "…").c_str()) > width) {
        do { value.pop_back(); } while (!value.empty() && (static_cast<unsigned char>(value.back()) & 0xc0) == 0x80);
        // Remove the leading byte of a partially removed multibyte character.
        if (!value.empty() && static_cast<unsigned char>(value.back()) >= 0xc0) value.pop_back();
    }
    return value + "…";
}
}

} // namespace audio_converter::gui
