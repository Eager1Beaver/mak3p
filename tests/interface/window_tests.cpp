#include "gui/main_window.hpp"
#include "gui/constants.hpp"
#include "test_support.hpp"
#include <FL/Fl.H>
#include <FL/Fl_Help_View.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Tooltip.H>
#include <fstream>

using namespace audio_converter;
using namespace std::chrono_literals;
int main(int argc, char** argv) {
    try {
        test::TempDirectory tmp;
        fs::create_directory(tmp.path / "documents");
        const fs::path documents = MAK3P_DOCUMENT_DIRECTORY;
        fs::copy_file(documents / "about.html", tmp.path / "documents" / "about.html");
        fs::copy_file(documents / "help.html", tmp.path / "documents" / "help.html");
        BatchBackend backend{
            [](const SourceFile& source, const AudioSettings& settings, std::stop_token) {
                Inspection input; input.source = source; input.settings = settings;
                input.state = source.path.stem() == "unsupported" ? JobState::UnsupportedSkipped : JobState::Ready;
                input.diagnostic = {input.state == JobState::Ready ? "ready" : "no_audio", {}, "Test explanation"};
                MediaInfo media; media.audio.codec = "flac"; media.audio.sample_rate = 96000;
                media.audio.channels = 2; media.bitrate_bps = 1280000; media.duration_seconds = 312;
                input.media = media; input.source_size = 49920000; return input;
            },
            [](const Inspection&, const AudioSettings&, OutputTransaction&, std::stop_token stop, ProgressCallback progress) {
                while (!stop.stop_requested()) { if (progress) progress({0.25, 1s}); std::this_thread::sleep_for(10ms); }
                return ConversionResult{JobState::Cancelled, {"cancelled", {}, {}}, {}, {}};
            }, "ffmpeg version fixture", "ffprobe version fixture"
        };
        gui::MainWindow window(tmp.path, backend); window.show();
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        while (window.controller().snapshot().phase != GuiPhase::Idle) {
            Fl::wait(0.01); if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("GUI initialization timed out");
        }
        window.refresh(); Fl::flush();
        test::require(window.w() == 500 && window.h() == 640, "Default window size differs");
        const int footer = window.h() - gui::metrics::settings_panel_height;
        int footer_bottom = 0;
        int start_top = 0;
        for (int index = 0; index < window.children(); ++index) {
            auto* widget = window.child(index);
            if (widget->y() == footer + gui::metrics::notice_offset)
                footer_bottom = std::max(footer_bottom, widget->y() + widget->h());
            if (widget->label() && gui::text("gui_start") == widget->label()) start_top = widget->y();
        }
        test::require(footer_bottom > 0 && start_top - footer_bottom >= gui::metrics::gap,
                      "Notice or engine controls overlap Start conversion");
        const auto queue_height = window.queue_view().h();
        std::vector<int> heights; for (int i = 0; i < window.children(); ++i) heights.push_back(window.child(i)->h());
        window.size(800, 850); window.refresh(); Fl::flush();
        test::require(window.queue_view().h() == queue_height + 210 && window.queue_view().w() == 776, "Queue did not absorb window growth");
        for (int i = 0; i < window.children(); ++i) {
            auto* child = window.child(i);
            if (child != &window.queue_view()) test::require(child->h() == heights[i], "Control stretched vertically");
            test::require(child->x() >= 0 && child->y() >= 0 && child->x() + child->w() <= window.w() && child->y() + child->h() <= window.h(), "Control outside window bounds");
        }
        window.size(500, 640);
        for (int i = 0; i < 30; ++i) Fl::wait(0.01);
        test::require(window.w() == 500 && window.h() == 640, "Window did not restore its minimum size");
        std::vector<fs::path> files{tmp.path / "unsupported.flac"};
        for (int i = 0; i < 100; ++i) { auto file = tmp.path / fs::path(u8"音楽 - A very long performer and track name for clipping "); file += std::to_string(i) + ".flac"; files.push_back(file); }
        test::require(window.controller().add(std::move(files)), "Window import rejected");
        while (window.controller().snapshot().phase != GuiPhase::Idle || window.controller().snapshot().batch.jobs.size() != 101) {
            Fl::wait(0.01); if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("GUI inspection timed out");
        }
        window.refresh(); Fl::flush(); window.make_current();
        auto* pixels = fl_read_image(nullptr, 0, 0, window.w(), window.h());
        if (argc > 1 && pixels) {
            std::ofstream image(fs::path(argv[1]) / "window.ppm", std::ios::binary);
            image << "P6\n" << window.w() << ' ' << window.h() << "\n255\n";
            image.write(reinterpret_cast<char*>(pixels), window.w() * window.h() * 3);
        }
        delete[] pixels;
        Fl::e_x = window.queue_view().x() + 370; Fl::e_y = window.queue_view().y() + 19;
        Fl::e_x_root = window.x() + Fl::e_x; Fl::e_y_root = window.y() + Fl::e_y;
        Fl_Tooltip::delay(0.0); window.queue_view().handle(FL_MOVE); Fl::flush();
        test::require(Fl_Tooltip::current_window() && Fl_Tooltip::current_window()->shown(), "Status hover did not show an explanation tooltip");
        window.queue_view().handle(FL_LEAVE); Fl_Tooltip::delay(0.5);
        auto* scrollbar = dynamic_cast<Fl_Scrollbar*>(window.queue_view().child(0));
        test::require(scrollbar && scrollbar->visible(), "Long selection has no scrollbar");
        scrollbar->value(300); window.queue_view().redraw(); Fl::flush();
        // Invoke the actual menu callbacks and verify separate auxiliary windows.
        auto* menu = dynamic_cast<Fl_Menu_Button*>(window.child(0));
        test::require(menu != nullptr, "Application menu missing");
        menu->menu()[0].do_callback(menu); Fl::check();
        Fl_Window* about = nullptr;
        for (auto* candidate = Fl::first_window(); candidate; candidate = Fl::next_window(candidate))
            if (candidate != &window && candidate->shown() && candidate->label() && std::string(candidate->label()) == gui::text("gui_about")) about = candidate;
        test::require(about != nullptr, "About did not open a separate window");
        auto* about_header = dynamic_cast<Fl_Box*>(about->child(1));
        auto* about_body = dynamic_cast<Fl_Help_View*>(about->child(2));
        test::require(about_header && about_header->label() &&
            std::string_view(about_header->label()).find(release_date) != std::string_view::npos,
            "About header did not show the configured release date");
        test::require(about_body && about_body->filename() && about_body->value() &&
            std::string_view(about_body->filename()).ends_with("about.html") &&
            std::string_view(about_body->value()).find("https://github.com/") != std::string_view::npos,
            "About did not load the generated document with its GitHub link");
        test::require(!about_body->scrollbar()->visible() && !about_body->hscrollbar()->visible(),
                      "The standard About content requires scrolling");
        about->hide();
        menu->menu()[1].do_callback(menu); Fl::check();
        Fl_Window* help = nullptr;
        for (auto* candidate = Fl::first_window(); candidate; candidate = Fl::next_window(candidate))
            if (candidate != &window && candidate->shown() && candidate->label() && std::string(candidate->label()) == gui::text("gui_help")) help = candidate;
        test::require(help != nullptr, "Help did not open a separate window"); help->hide();
        test::require(window.controller().start(), "GUI start failed");
        while (!window.controller().snapshot().batch.counts.converting) Fl::wait(0.01);
        window.refresh(); Fl::flush();
        test::require(!window.controller().add({tmp.path / "blocked.flac"}), "GUI allowed import during conversion");
        window.controller().cancel();
        while (window.controller().snapshot().phase != GuiPhase::Idle) Fl::wait(0.01);
        test::require(window.controller().snapshot().batch.counts.cancelled == 100, "GUI cancellation lost pending rows");
        window.controller().close();
        while (window.controller().snapshot().phase != GuiPhase::Closed) Fl::wait(0.01);
        window.hide();
        std::cout << "GUI sizing, 101-row import, skip presentation, menus, drawing, cancellation, and shutdown passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
