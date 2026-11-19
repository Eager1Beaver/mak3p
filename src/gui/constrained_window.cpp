#include "gui/constrained_window.hpp"

#include <FL/Fl.H>
#if defined(FLTK_USE_X11)
#include <FL/platform.H>
#endif
#include <algorithm>
#include <cmath>

namespace audio_converter::gui {
ConstrainedWindow::ConstrainedWindow(int width, int height, const char* title, bool fixed)
    : Fl_Double_Window(width, height, title), minimum_width_(width), minimum_height_(height), fixed_(fixed) {}

void ConstrainedWindow::publish_size_limits() {
    if (fixed_) resizable(nullptr);
    size_range(minimum_width_, minimum_height_, fixed_ ? minimum_width_ : 0, fixed_ ? minimum_height_ : 0);
}

void ConstrainedWindow::resize(int x, int y, int width, int height) {
    const int bounded_width = fixed_ ? minimum_width_ : std::max(width, minimum_width_);
    const int bounded_height = fixed_ ? minimum_height_ : std::max(height, minimum_height_);
    Fl_Double_Window::resize(x, y, bounded_width, bounded_height);
#if defined(FLTK_USE_X11)
    if (shown() && (width != bounded_width || height != bounded_height)) {
        // A ConfigureNotify can report a smaller server window while the client size
        // already equals its minimum, making the normal FLTK resize a no-op.
        const float scale = Fl::screen_scale(screen_num());
        XResizeWindow(fl_x11_display(), fl_x11_xid(this),
                      static_cast<unsigned>(std::lround(bounded_width * scale)),
                      static_cast<unsigned>(std::lround(bounded_height * scale)));
        XFlush(fl_x11_display());
    }
#endif
}
} // namespace audio_converter::gui
