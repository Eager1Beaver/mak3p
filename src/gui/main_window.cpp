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
    : ConstrainedWindow(metrics::minimum_width,
                       metrics::minimum_height,
                       application_name),
        directory_(std::move(directory)), controller_(directory_, std::move(backend)) {
    install_icon();
    color(colors::background);
    menu_ = new Fl_Menu_Button(0, 0, 1, 1, application_name);
    menu_->add(text("gui_about").c_str(), 0, [](Fl_Widget*, void* data) { static_cast<MainWindow*>(data)->dialog(true); }, this);
    menu_->add(text("gui_help").c_str(), 0, [](Fl_Widget*, void* data) { static_cast<MainWindow*>(data)->dialog(false); }, this);
    menu_->add(text("gui_select_files").c_str(), 0, [](Fl_Widget*, void* data) { static_cast<MainWindow*>(data)->choose(false); }, this);
    menu_->add(text("gui_select_folders").c_str(), 0, [](Fl_Widget*, void* data) { static_cast<MainWindow*>(data)->choose(true); }, this);
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
    end(); resizable(queue_); layout(); publish_size_limits();
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
    ConstrainedWindow::resize(x, y, w, h); layout();
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
void MainWindow::settings_changed(Fl_Widget* source) {
    if (snapshot_.phase != GuiPhase::Idle) return;
    auto settings = snapshot_.settings;
    settings.audio.bitrate_kbps = rates[std::clamp(bitrate_->value(), 0, 5)];
    settings.audio.channel_mode = channels_->value() == 0 ? ChannelMode::Stereo : channels_->value() == 1 ? ChannelMode::Mono : ChannelMode::Auto;
    if (output_->value() == 1 && (settings.output_mode != OutputMode::CustomDirectory || source == output_)) {
        Fl_Native_File_Chooser picker(Fl_Native_File_Chooser::BROWSE_DIRECTORY); picker.title(text("gui_choose_folder").c_str());
        const int result = picker.show();
        if (result != 0) { populate(snapshot_.settings); return; }
        settings.custom_output_directory = path_from_utf8(picker.filename());
    }
    settings.output_mode = output_->value() == 1 ? OutputMode::CustomDirectory : OutputMode::SameAsSource;
    settings.workers = worker_values_[std::clamp(workers_->value(), 0, static_cast<int>(worker_values_.size()) - 1)];
    settings.audio.include_mp3_from_folders = include_->value(); settings.audio.force_upsampling = force_->value(); settings.logging_enabled = log_->value();
    if (!controller_.configure(settings)) populate(snapshot_.settings);
}
void MainWindow::choose(bool folders) {
    try {
        if (snapshot_.phase != GuiPhase::Idle) return;
        Fl_Native_File_Chooser picker(folders ? Fl_Native_File_Chooser::BROWSE_MULTI_DIRECTORY : Fl_Native_File_Chooser::BROWSE_MULTI_FILE);
        const auto title = text(folders ? "gui_select_folders" : "gui_select_files"); picker.title(title.c_str());
        const auto filter = text("gui_files_filter"); if (!folders) picker.filter(filter.c_str());
        const int result = picker.show();
        if (result < 0) { fl_alert("%s", picker.errmsg()); return; } if (result > 0) return;
        std::vector<fs::path> paths; for (int i = 0; i < picker.count(); ++i) paths.push_back(path_from_utf8(picker.filename(i)));
        (void)controller_.add(std::move(paths));
    } catch (const std::exception& error) {
        fl_alert("%s\n%s", text("gui_operation_failed").c_str(), error.what());
    }
}
void MainWindow::choose_engine() {
    try {
        if (snapshot_.phase != GuiPhase::EngineMissing) return;
        const int answer = fl_choice("%s", text("gui_exit").c_str(), text("gui_choose_engine").c_str(), text("gui_retry").c_str(), text("engine_missing").c_str());
        if (answer == 0) { request_close(); return; }
        if (answer == 2) { controller_.select_engine({}); return; }
        Fl_Native_File_Chooser picker(Fl_Native_File_Chooser::BROWSE_DIRECTORY); picker.title(text("gui_choose_engine").c_str());
        if (!picker.show()) controller_.select_engine(path_from_utf8(picker.filename()));
    } catch (const std::exception& error) {
        fl_alert("%s\n%s", text("gui_operation_failed").c_str(), error.what());
    }
}
void MainWindow::drop(std::string_view payload) {
    if (snapshot_.phase != GuiPhase::Idle) return;
    try { (void)controller_.add(dropped_paths(payload)); }
    catch (const std::exception& error) { fl_alert("%s", error.what()); }
}
int MainWindow::handle(int event) {
    if (event == FL_DND_ENTER || event == FL_DND_DRAG || event == FL_DND_RELEASE) {
        if (closing_ || snapshot_.phase != GuiPhase::Idle) return 0;
        Fl::belowmouse(this);
        return 1;
    }
    if (event == FL_DND_LEAVE) return 1;
    if (event == FL_PASTE) { drop(std::string_view(Fl::event_text(), Fl::event_length())); return 1; }
    return Fl_Double_Window::handle(event);
}
void MainWindow::request_close() {
    if (closing_) return;
    closing_ = true; controller_.window_size(normal_size_.width, normal_size_.height); controller_.close(); refresh();
}
void MainWindow::timer(void* data) {
    auto* window = static_cast<MainWindow*>(data); window->refresh();
    if (window->shown()) Fl::repeat_timeout(metrics::refresh_seconds, timer, data);
}
void MainWindow::refresh() {
    std::erase_if(dialogs_, [](const auto& item) { return !item->shown(); });
    snapshot_ = controller_.snapshot();
    if (!restored_ && snapshot_.phase != GuiPhase::Initializing) {
        restored_ = true;
        int screen_x, screen_y, screen_w, screen_h; Fl::screen_work_area(screen_x, screen_y, screen_w, screen_h, 0);
        size(std::max(metrics::minimum_width, std::min(snapshot_.settings.window.width, screen_w)),
             std::max(metrics::minimum_height, std::min(snapshot_.settings.window.height, screen_h)));
        populate(snapshot_.settings);
    }
    if (snapshot_.phase == GuiPhase::Closed) { for (auto& window : dialogs_) window->hide(); hide(); return; }
    const bool idle = !closing_ && snapshot_.phase == GuiPhase::Idle;
    enabled(add_, idle); enabled(clear_, idle && !snapshot_.batch.jobs.empty());
    for (Fl_Widget* widget : std::array<Fl_Widget*, 7>{bitrate_, channels_, output_, workers_, include_, force_, log_}) enabled(widget, idle);
    enabled(start_, !closing_ && (snapshot_.can_start || snapshot_.phase == GuiPhase::Running));
    update_label(start_, text(closing_ ? "gui_closing" : snapshot_.phase == GuiPhase::Running ? "gui_cancel" : snapshot_.phase == GuiPhase::Cancelling ? "gui_cancelling" : "gui_start"));
    std::vector<RowText> rows; for (std::size_t i = 0; i < snapshot_.batch.jobs.size(); ++i) rows.push_back(present_row(snapshot_.batch.jobs[i], i + 1, snapshot_.settings));
    queue_->rows(std::move(rows), idle);
    if (idle) {
        output_->replace(1, snapshot_.settings.output_mode == OutputMode::CustomDirectory ? path_utf8(snapshot_.settings.custom_output_directory.filename()).c_str() : text("gui_choose_folder").c_str());
        output_->copy_tooltip(path_utf8(snapshot_.settings.custom_output_directory).c_str());
        const auto& counts = snapshot_.batch.counts;
        if (snapshot_.has_run && counts.ready == 0) update_label(summary_, text("gui_counts", {{"completed", std::to_string(snapshot_.last_run.completed)}, {"skipped", std::to_string(snapshot_.last_run.skipped)}, {"failed", std::to_string(snapshot_.last_run.failed)}, {"cancelled", std::to_string(snapshot_.last_run.cancelled)}}));
        else update_label(summary_, text("gui_ready_count", {{"ready", std::to_string(counts.ready)}, {"total", std::to_string(counts.total)}}));
        // Synchronize values after an asynchronous settings command settles.
        if (bitrate_->value() != static_cast<int>(std::find(std::begin(rates), std::end(rates), snapshot_.settings.audio.bitrate_kbps) - std::begin(rates)) ||
            channels_->value() != (snapshot_.settings.audio.channel_mode == ChannelMode::Stereo ? 0 : snapshot_.settings.audio.channel_mode == ChannelMode::Mono ? 1 : 2)) populate(snapshot_.settings);
    } else update_label(summary_, phase_text(snapshot_.phase));
    std::size_t done = 0;
    for (const auto& row : snapshot_.batch.jobs) if (std::find(snapshot_.run_ids.begin(), snapshot_.run_ids.end(), row.job.id()) != snapshot_.run_ids.end() &&
        row.job.state() != JobState::Ready && row.job.state() != JobState::Converting && row.job.state() != JobState::Checking) ++done;
    update_label(count_, snapshot_.run_ids.empty() ? "" : text("gui_run_count", {{"done", std::to_string(done)}, {"total", std::to_string(snapshot_.run_ids.size())}}));
    std::string notice;
    if (!snapshot_.notices.empty()) notice = message(snapshot_.notices.back());
    else if (snapshot_.batch.counts.excluded) notice = text("gui_excluded", {{"count", std::to_string(snapshot_.batch.counts.excluded)}});
    update_label(notice_, notice);
    std::string full_notice; for (const auto& item : snapshot_.notices) full_notice += message(item) + "\n" + item.technical_detail + "\n";
    if (engine_tip_ != full_notice) { engine_tip_ = std::move(full_notice); notice_->copy_tooltip(engine_tip_.c_str()); }
    if (snapshot_.phase == GuiPhase::EngineMissing) { engine_->show(); notice_->hide(); } else { engine_->hide(); notice_->show(); }
    if (resize_pending_ && restored_ && !closing_ && std::chrono::steady_clock::now() - resized_ > std::chrono::milliseconds(metrics::resize_save_delay_ms)) {
        resize_pending_ = false; controller_.window_size(normal_size_.width, normal_size_.height);
    }
}
void MainWindow::open_document(const char* name) {
    auto window = std::make_unique<ConstrainedWindow>(metrics::help_width, metrics::help_height, nullptr, true);
    window->copy_label(text(std::string_view(name) == "licenses.html" ? "gui_license" : "gui_changelog").c_str());
    auto* body = new Fl_Help_View(metrics::margin, metrics::margin,
                                  metrics::help_width - 2 * metrics::margin, metrics::help_height - 64);
    body->textfont(metrics::font_face); body->textsize(metrics::font_size);
    body->textcolor(colors::text); body->color(colors::background);
    const auto path = (directory_ / "documents" / name).generic_u8string();
    if (body->load(reinterpret_cast<const char*>(path.c_str())) != 0)
        body->value(("<p>" + escape_html(text("gui_document_failed")) + "</p>").c_str());
    auto* close = new Fl_Button(metrics::help_width - 104, metrics::help_height - 42, 92, 28);
    close->copy_label(text("gui_close").c_str());
    close->callback([](Fl_Widget* widget, void*) { widget->window()->hide(); });
    window->end(); window->publish_size_limits();
    window->show();
    // Keep hidden windows alive until the next event-loop tick; callbacks can still
    // be executing on a window whose child has just opened a document.
    dialogs_.push_back(std::move(window));
}
void MainWindow::dialog(bool about) {
    const int width = about ? metrics::about_width : metrics::help_width;
    const int height = about ? metrics::about_height : metrics::help_height;
    auto window = std::make_unique<ConstrainedWindow>(width, height, nullptr, true);
    window->copy_label(text(about ? "gui_about" : "gui_help").c_str()); window->set_non_modal();
    auto* body = new Fl_Help_View(metrics::margin, metrics::margin, width - 2 * metrics::margin, height - 64);
    body->textfont(metrics::font_face); body->textsize(metrics::font_size);
    body->textcolor(colors::text); body->color(colors::background);
    std::string content;
    if (about) {
#ifdef _WIN32
        const auto platform = "Windows";
#elif defined(__APPLE__)
        const auto platform = "macOS";
#else
        const auto platform = "Linux";
#endif
        content = "<table width=100%><tr><td><h2>" + std::string(application_name) +
            "</h2></td><td align=right>" + release_version + "<br>" +
            text("gui_build", {{"platform", platform}, {"bits", std::to_string(sizeof(void*) * 8)}}) + "<br>" + text("gui_release") +
            "</td></tr></table><p>" + escape_html(text("gui_author")) + "</p><p>" + escape_html(text("gui_contacts")) + "</p>";
    } else {
        const auto path = (directory_ / "documents" / "help.html").generic_u8string();
        if (body->load(reinterpret_cast<const char*>(path.c_str())) != 0)
            body->value(("<p>" + escape_html(text("gui_document_failed")) + "</p>").c_str());
        auto* copyright = new Fl_Box(12, height - 42, width - 128, 28);
        copyright->copy_label(text("gui_copyright").c_str()); copyright->labelsize(metrics::small_font_size);
        copyright->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    }
    if (about) body->value(content.c_str());
    if (about) {
        auto* changelog = new Fl_Button(12, height - 42, 112, 28); changelog->copy_label(text("gui_changelog").c_str());
        changelog->box(FL_FLAT_BOX); changelog->labelcolor(colors::accent);
        changelog->callback([](Fl_Widget*, void* data) { static_cast<MainWindow*>(data)->open_document("changelog.html"); }, this);
        auto* license = new Fl_Button(width - 204, height - 42, 88, 28); license->copy_label(text("gui_license").c_str());
        license->box(FL_FLAT_BOX); license->labelcolor(colors::accent);
        license->callback([](Fl_Widget*, void* data) { static_cast<MainWindow*>(data)->open_document("licenses.html"); }, this);
    }
    auto* close = new Fl_Button(width - 104, height - 42, 92, 28); close->copy_label(text("gui_close").c_str());
    close->callback([](Fl_Widget* widget, void*) { widget->window()->hide(); });
    window->end(); window->publish_size_limits(); window->show();
    dialogs_.push_back(std::move(window));
}


} // namespace audio_converter::gui
