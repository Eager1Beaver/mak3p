#include "gui/main_window.hpp"
#include "gui/constants.hpp"
#include "app/import.hpp"
#include "audio/process.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Help_View.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_ask.H>
#include <FL/filename.H>
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>

namespace audio_converter::gui {
namespace {
constexpr int rates[] = {320, 256, 224, 192, 160, 128};
void enabled(Fl_Widget* widget, bool value) { if (value) widget->activate(); else widget->deactivate(); }
void update_label(Fl_Widget* widget, const std::string& value) {
    if (!widget->label() || value != widget->label()) widget->copy_label(value.c_str());
}
std::string escape_html(std::string_view input) {
    std::string output;
    for (const char c : input) {
        if (c == '&') output += "&amp;";
        else if (c == '<') output += "&lt;";
        else if (c == '>') output += "&gt;";
        else if (c == '\n') output += "<br>";
        else output += c;
    }
    return output;
}
void install_icon() {
    static const auto pixels = [] {
        std::array<unsigned char, 32 * 32 * 3> data{};
        for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) {
            const bool mark = (x >= 7 && x <= 10 && y >= 8 && y < 25) ||
                (x >= 21 && x <= 24 && y >= 8 && y < 25) ||
                (y >= 8 && y <= 11 && x >= 7 && x <= 24) ||
                (y >= 14 && y <= 17 && x >= 14 && x <= 17);
            const int offset = (y * 32 + x) * 3;
            data[offset] = mark ? 255 : 44; data[offset + 1] = mark ? 255 : 100; data[offset + 2] = mark ? 255 : 170;
        }
        return data;
    }();
    static const Fl_RGB_Image icon(pixels.data(), 32, 32, 3);
    Fl_Window::default_icon(&icon);
}
}

} // namespace audio_converter::gui
