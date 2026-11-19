#pragma once

#include <FL/Fl_Double_Window.H>

namespace audio_converter::gui {
// Publishes native size limits and restores them when a compositor ignores its hints.
class ConstrainedWindow : public Fl_Double_Window {
public:
    ConstrainedWindow(int width, int height, const char* title = nullptr, bool fixed = false);
    void publish_size_limits();
    void resize(int x, int y, int width, int height) override;
private:
    int minimum_width_, minimum_height_;
    bool fixed_;
};
} // namespace audio_converter::gui
