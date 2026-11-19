#pragma once
#include "gui/queue_view.hpp"
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Menu_Button.H>

namespace audio_converter::gui {
class MainWindow : public Fl_Double_Window {
public:
    explicit MainWindow(fs::path directory, std::optional<BatchBackend> backend = std::nullopt);
    ~MainWindow() override;
    void resize(int x, int y, int w, int h) override;
    int handle(int event) override;
    void refresh();
    [[nodiscard]] GuiController& controller() { return controller_; }
    [[nodiscard]] QueueView& queue_view() { return *queue_; }
private:
    fs::path directory_;
    GuiController controller_;
    GuiSnapshot snapshot_;
    Fl_Menu_Button *menu_, *add_;
    QueueView* queue_;
    Fl_Button *clear_, *start_, *engine_;
    Fl_Choice *bitrate_, *channels_, *output_, *workers_;
    Fl_Check_Button *include_, *force_, *log_;
    Fl_Box *hint_, *metadata_, *summary_, *count_, *notice_;
    std::vector<unsigned> worker_values_;
    std::vector<std::unique_ptr<Fl_Window>> dialogs_;
    bool restored_ = false;
    bool closing_ = false;
    bool resize_pending_ = false;
    WindowPreferences normal_size_;
    std::chrono::steady_clock::time_point resized_;
    std::string engine_tip_;
    void layout();
    void settings_changed(Fl_Widget* source);
    void choose(bool folders);
    void choose_engine();
    void drop(std::string_view payload);
    void request_close();
    void dialog(bool about);
    void open_document(const char* name);
    void populate(const AppSettings& settings);
    static void timer(void* data);
};
} // namespace audio_converter::gui
