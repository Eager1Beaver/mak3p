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
MainWindow::MainWindow(fs::path directory, std::optional<BatchBackend> backend)
    : Fl_Double_Window(metrics::minimum_width, 
                       metrics::minimum_height, 
                       application_name),
        directory_(std::move(directory)), controller_(directory_, std::move(backend)) {
    install_icon();
    color(colors::background);
    size_range(metrics::minimum_width, metrics::minimum_height);
    menu_ = new Fl_Menu_Button(0, 0, 1, 1, application_name);
    menu_->add(text("gui_about").c_str(), 0, [](Fl_Widget*, void* data) { static_cast<MainWindow*>(data)->dialog(true); }, this);
    menu_->add(text("gui_help").c_str(), 0, [](Fl_Widget*, void* data) { static_cast<MainWindow*>(data)->dialog(false); }, this);
    menu_->add(text("gui_add").c_str(), 0, [](Fl_Widget*, void* data) { auto* window = static_cast<MainWindow*>(data); if (window->snapshot_.phase == GuiPhase::Idle) window->add_->popup(); }, this);
    menu_->add(text("gui_default_size").c_str(), 0, [](Fl_Widget*, void* data) { auto* window = static_cast<MainWindow*>(data); window->un_maximize(); window->size(metrics::minimum_width, metrics::minimum_height); }, this);
    menu_->add(text("gui_exit").c_str(), 0, [](Fl_Widget*, void* data) { static_cast<MainWindow*>(data)->request_close(); }, this);
    queue_ = new QueueView(0, 0, 1, 1);
    queue_->remove = [this](JobId id) { (void)controller_.remove(id); };
    queue_->drop = [this](std::string_view payload) { drop(payload); };
    hint_ = new Fl_Box(0, 0, 1, 1); hint_->copy_label(text("gui_hint").c_str()); hint_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    add_ = new Fl_Menu_Button(0, 0, 1, 1); add_->copy_label(text("gui_add").c_str());
    add_->add(text("gui_select_files").c_str(), 0, [](Fl_Widget*, void* data) { static_cast<MainWindow*>(data)->choose(false); }, this);
    add_->add(text("gui_select_folders").c_str(), 0, [](Fl_Widget*, void* data) { static_cast<MainWindow*>(data)->choose(true); }, this);
    clear_ = new Fl_Button(0, 0, 1, 1); clear_->copy_label(text("gui_clear").c_str());
    clear_->callback([](Fl_Widget*, void* data) { (void)static_cast<MainWindow*>(data)->controller_.clear(); }, this);
    auto choice = [this](const char* key) { auto* widget = new Fl_Choice(0, 0, 1, 1);
        widget->copy_label(text(key).c_str()); widget->align(FL_ALIGN_TOP_LEFT);
        widget->callback([](Fl_Widget* source, void* data) { static_cast<MainWindow*>(data)->settings_changed(source); }, this); return widget; };
    bitrate_ = choice("gui_bitrate"); for (int rate : rates) bitrate_->add(text("gui_kbps", {{"value", std::to_string(rate)}}).c_str());
    channels_ = choice("gui_channels"); for (const char* key : {"gui_stereo", "gui_mono", "gui_auto"}) channels_->add(text(key).c_str());
    output_ = choice("gui_output"); output_->add(text("gui_source_folders").c_str()); output_->add(text("gui_choose_folder").c_str()); output_->when(FL_WHEN_RELEASE_ALWAYS);
    workers_ = choice("gui_workers"); workers_->copy_tooltip(text("gui_workers_tip").c_str());
    auto check = [this](const char* key, const char* tip) { auto* widget = new Fl_Check_Button(0, 0, 1, 1);
        widget->copy_label(text(key).c_str()); widget->copy_tooltip(text(tip).c_str());
        widget->callback([](Fl_Widget* source, void* data) { static_cast<MainWindow*>(data)->settings_changed(source); }, this); return widget; };
    include_ = check("gui_include_mp3", "gui_include_tip"); force_ = check("gui_force", "gui_force_tip"); log_ = check("gui_log", "gui_log_tip");
    metadata_ = new Fl_Box(0, 0, 1, 1); metadata_->copy_label(text("gui_metadata").c_str());
    metadata_->copy_tooltip(text("gui_metadata_detail").c_str()); metadata_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    summary_ = new Fl_Box(0, 0, 1, 1); summary_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    count_ = new Fl_Box(0, 0, 1, 1); count_->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
    notice_ = new Fl_Box(0, 0, 1, 1); notice_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP); notice_->labelcolor(fl_rgb_color(135, 75, 15));
    engine_ = new Fl_Button(0, 0, 1, 1); engine_->copy_label(text("gui_engine_action").c_str()); engine_->hide();
    engine_->callback([](Fl_Widget*, void* data) { static_cast<MainWindow*>(data)->choose_engine(); }, this);
    start_ = new Fl_Button(0, 0, 1, 1); start_->color(colors::accent); start_->labelcolor(FL_WHITE);
    start_->callback([](Fl_Widget*, void* data) { auto* window = static_cast<MainWindow*>(data);
        if (window->snapshot_.phase == GuiPhase::Running) window->controller_.cancel(); else (void)window->controller_.start(); }, this);
    for (int i = 0; i < children(); ++i) {
        child(i)->labelsize(metrics::font_size); child(i)->labelfont(metrics::font_face);
        child(i)->box(FL_FLAT_BOX); if (child(i) != start_ && child(i) != queue_) child(i)->color(colors::control);
    }
    for (auto* widget : {hint_, metadata_, notice_, count_, summary_}) { widget->labelsize(metrics::small_font_size); widget->color(color()); }
    for (auto* widget : {bitrate_, channels_, output_, workers_}) { widget->textsize(metrics::font_size); widget->box(FL_BORDER_BOX); }
    menu_->box(FL_THIN_UP_BOX); add_->box(FL_THIN_UP_BOX); clear_->box(FL_THIN_UP_BOX); start_->box(FL_THIN_UP_BOX);
    end(); resizable(queue_); layout();
    callback([](Fl_Widget*, void* data) { static_cast<MainWindow*>(data)->request_close(); }, this);
    populate(AppSettings{}); refresh(); Fl::add_timeout(metrics::refresh_seconds, timer, this);
}
MainWindow::~MainWindow() { Fl::remove_timeout(timer, this); controller_.close(); }
void MainWindow::layout() {
    using namespace metrics;
    const int width = w() - 2 * margin, col = (width - gap) / 2;
    menu_->resize(margin, margin, menu_width, menu_height);
    const int queue_top = margin + menu_height + gap;
    const int bottom = h() - settings_panel_height;
    queue_->resize(margin, queue_top, width, bottom - queue_top);
    hint_->resize(margin, bottom + hint_offset, width, hint_height);
    add_->resize(margin, bottom + input_buttons_offset, col, control_height); clear_->resize(margin + col + gap, bottom + input_buttons_offset, col, control_height);
    bitrate_->resize(margin, bottom + audio_settings_offset, col, control_height); channels_->resize(margin + col + gap, bottom + audio_settings_offset, col, control_height);
    output_->resize(margin, bottom + output_settings_offset, col, control_height); workers_->resize(margin + col + gap, bottom + output_settings_offset, col, control_height);
    include_->resize(margin, bottom + import_options_offset, col, option_height); force_->resize(margin + col + gap, bottom + import_options_offset, col, option_height);
    log_->resize(margin, bottom + log_option_offset, width, option_height); metadata_->resize(margin, bottom + metadata_offset, width, hint_height);
    summary_->resize(margin, bottom + summary_offset, width - count_width - 2, option_height); count_->resize(w() - margin - count_width, bottom + summary_offset, count_width, option_height);
    notice_->resize(margin, bottom + notice_offset, width, hint_height); engine_->resize(margin, bottom + engine_action_offset, width, 24);
    start_->resize(margin, h() - margin - primary_height, width, primary_height);
}
void MainWindow::resize(int x, int y, int w, int h) {
    Fl_Double_Window::resize(x, y, std::max(w, metrics::minimum_width), std::max(h, metrics::minimum_height)); layout();
    if (!maximize_active() && !fullscreen_active()) {
        normal_size_ = {this->w(), this->h()};
        resized_ = std::chrono::steady_clock::now(); resize_pending_ = true;
    }
}
void MainWindow::populate(const AppSettings& settings) {
    const auto found = std::find(std::begin(rates), std::end(rates), settings.audio.bitrate_kbps);
    bitrate_->value(static_cast<int>(found - std::begin(rates)));
    channels_->value(settings.audio.channel_mode == ChannelMode::Stereo ? 0 : settings.audio.channel_mode == ChannelMode::Mono ? 1 : 2);
    output_->value(settings.output_mode == OutputMode::SameAsSource ? 0 : 1);
    worker_values_ = worker_options(std::thread::hardware_concurrency(), settings.workers); workers_->clear();
    for (auto value : worker_values_) workers_->add(value ? std::to_string(value).c_str() : text("gui_auto").c_str());
    const auto selected = std::find(worker_values_.begin(), worker_values_.end(), settings.workers);
    workers_->value(selected == worker_values_.end() ? 0 : static_cast<int>(selected - worker_values_.begin()));
    include_->value(settings.audio.include_mp3_from_folders); force_->value(settings.audio.force_upsampling); log_->value(settings.logging_enabled);
    output_->replace(1, settings.output_mode == OutputMode::CustomDirectory ? path_utf8(settings.custom_output_directory.filename()).c_str() : text("gui_choose_folder").c_str());
    output_->copy_tooltip((settings.custom_output_directory.empty() ? text("gui_source_folders") : path_utf8(settings.custom_output_directory)).c_str());
}

} // namespace audio_converter::gui
