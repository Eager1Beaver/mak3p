#include "gui/main_window.hpp"
#include "app/import.hpp"
#include "audio/process.hpp"
#include "test_support.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Help_View.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_ICO_Image.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>
#include <X11/Xatom.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <tuple>
#include <unistd.h>

using namespace audio_converter;
using namespace std::chrono_literals;
using test::require;

namespace {
template<class Predicate> void await(Predicate predicate, std::chrono::seconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        Fl::wait(0.01);
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Native GUI operation timed out");
    }
}
BatchBackend backend() {
    return {
        [](const SourceFile& source, const AudioSettings& settings, std::stop_token) {
            Inspection value; value.source = source; value.settings = settings;
            value.state = JobState::Ready; value.diagnostic = {"ready", {}, {}}; return value;
        },
        [](const Inspection&, const AudioSettings&, OutputTransaction&, std::stop_token, ProgressCallback) {
            return ConversionResult{};
        }, "ffmpeg version fixture", "ffprobe version fixture"
    };
}
Fl_Window* shown_window(const std::string& title) {
    for (auto* window = Fl::first_window(); window; window = Fl::next_window(window))
        if (window->shown() && window->label() && window->label() == title) return window;
    return nullptr;
}
void menu_action(gui::MainWindow& window, const char* key) {
    auto* menu = dynamic_cast<Fl_Menu_Button*>(window.child(0));
    const auto* item = menu->find_item(gui::text(key).c_str());
    require(item != nullptr, "Application menu action missing"); item->do_callback(menu);
}
void button_action(Fl_Window& window, const char* key) {
    for (int index = 0; index < window.children(); ++index) {
        auto* widget = window.child(index);
        if (widget->label() && widget->label() == gui::text(key)) { widget->do_callback(); return; }
    }
    throw std::runtime_error("Button action missing");
}
void close(gui::MainWindow& window) {
    window.controller().close(); await([&] { return window.controller().snapshot().phase == GuiPhase::Closed; }); window.hide();
}
void check_original_icon(Fl_Window& window) {
    Atom type; int format; unsigned long count, remaining; unsigned char* data = nullptr;
    require(XGetWindowProperty(fl_x11_display(), fl_x11_xid(&window),
        XInternAtom(fl_x11_display(), "_NET_WM_ICON", False), 0, 100000, False,
        XA_CARDINAL, &type, &format, &count, &remaining, &data) == Success &&
        type == XA_CARDINAL && format == 32 && remaining == 0 && data,
        "Window has no native icon property");
    std::unique_ptr<unsigned char, decltype(&XFree)> owner(data, XFree);
    const auto* pixels = reinterpret_cast<const unsigned long*>(data);
    Fl_ICO_Image directory(MAK3P_ICON_SOURCE, -2);
    require(directory.idcount() > 0, "Original icon contains no images");
    unsigned long offset = 0;
    for (int id = 0; id < directory.idcount(); ++id) {
        Fl_ICO_Image expected(MAK3P_ICON_SOURCE, id);
        require(!expected.fail() && expected.d() == 4, "Original icon did not decode as RGBA");
        require(offset + 2 <= count, "Native icon is missing its dimensions");
        require(pixels[offset] == static_cast<unsigned long>(expected.w()) &&
                pixels[offset + 1] == static_cast<unsigned long>(expected.h()),
                "Native icon dimensions differ at frame " + std::to_string(id));
        offset += 2;
        const auto size = static_cast<unsigned long>(expected.w() * expected.h());
        require(offset + size <= count, "Native icon property is truncated");
        for (unsigned long i = 0; i < size; ++i) {
            const auto* rgba = expected.array + i * 4;
            const unsigned long argb = (static_cast<unsigned long>(rgba[3]) << 24) |
                (static_cast<unsigned long>(rgba[0]) << 16) |
                (static_cast<unsigned long>(rgba[1]) << 8) | rgba[2];
            require((pixels[offset + i] & 0xffffffffUL) == argb,
                    "Native icon pixels differ at frame " + std::to_string(id) + ", pixel " + std::to_string(i));
        }
        offset += size;
    }
    require(offset == count, "Unexpected native icons remain");
}
void original_icons() {
    test::TempDirectory tmp;
    gui::MainWindow window(tmp.path, backend()); window.show();
    await([&] { return window.controller().snapshot().phase == GuiPhase::Idle; });
    check_original_icon(window);
    menu_action(window, "gui_about");
    auto* about = shown_window(gui::text("gui_about")); require(about, "About missing");
    check_original_icon(*about);
    button_action(*about, "gui_license");
    auto* license = shown_window(gui::text("gui_license")); require(license, "License missing");
    check_original_icon(*license); license->hide(); about->hide();
    menu_action(window, "gui_help");
    auto* help = shown_window(gui::text("gui_help")); require(help, "Help missing");
    check_original_icon(*help); help->hide(); close(window);
}

void check_native_size(Fl_Window& window, int width, int height) {
    const auto scale = Fl::screen_scale(window.screen_num());
    const int pixels_w = static_cast<int>(std::lround(width * scale));
    const int pixels_h = static_cast<int>(std::lround(height * scale));
    await([&] {
        XWindowAttributes attributes{}; XGetWindowAttributes(fl_x11_display(), fl_x11_xid(&window), &attributes);
        return attributes.width == pixels_w && attributes.height == pixels_h && window.w() == width && window.h() == height;
    });
}
void constraints_and_documents() {
    test::TempDirectory tmp; fs::create_directory(tmp.path / "documents");
    test::write(tmp.path / "documents" / "licenses.html", "<h1>License test content</h1>");
    test::write(tmp.path / "documents" / "changelog.html", "<h1>Changelog test content</h1>");
    gui::MainWindow window(tmp.path, backend()); window.show();
    await([&] { return window.controller().snapshot().phase == GuiPhase::Idle; }); window.refresh();
    XSizeHints hints{}; long supplied = 0;
    require(XGetWMNormalHints(fl_x11_display(), fl_x11_xid(&window), &hints, &supplied) &&
        (hints.flags & PMinSize) && hints.min_width >= 500 && hints.min_height >= 640, "Native minimum hints missing");
    XResizeWindow(fl_x11_display(), fl_x11_xid(&window), 90, 20); XFlush(fl_x11_display()); check_native_size(window, 500, 640);
    menu_action(window, "gui_about"); auto* about = shown_window(gui::text("gui_about")); require(about, "About missing");
    for (const auto* key : {"gui_license", "gui_changelog"}) {
        button_action(*about, key); auto* document = shown_window(gui::text(key)); require(document, "Document link opened no viewer");
        auto* body = dynamic_cast<Fl_Help_View*>(document->child(0));
        require(body && body->value() && std::strstr(body->value(), "test content"), "Document viewer did not load its file");
        XResizeWindow(fl_x11_display(), fl_x11_xid(document), 700, 800); XFlush(fl_x11_display());
        check_native_size(*document, gui::metrics::help_width, gui::metrics::help_height); document->hide();
    }
    for (const auto* key : {"gui_about", "gui_help"}) {
        if (std::string_view(key) == "gui_help") menu_action(window, key);
        auto* dialog = shown_window(gui::text(key)); require(dialog, "Auxiliary dialog missing");
        const int width = dialog->w(), height = dialog->h();
        require(dialog->resizable() == nullptr, "Fixed dialog contains a resizable target");
        require(XGetWMNormalHints(fl_x11_display(), fl_x11_xid(dialog), &hints, &supplied) &&
            (hints.flags & PMaxSize) && hints.min_width == hints.max_width && hints.min_height == hints.max_height, "Fixed native bounds missing");
        XResizeWindow(fl_x11_display(), fl_x11_xid(dialog), width + 200, height + 200); XFlush(fl_x11_display()); check_native_size(*dialog, width, height);
        XResizeWindow(fl_x11_display(), fl_x11_xid(dialog), 80, 30); XFlush(fl_x11_display()); check_native_size(*dialog, width, height); dialog->hide();
    }
    close(window);
}

void screenshot(Fl_Window& window, const char* name) {
    // Draw into an owned surface; a newly mapped X11 window may not have a back buffer yet.
    Fl_Image_Surface surface(window.w(), window.h()); Fl_Surface_Device::push_current(&surface);
    surface.draw(&window); std::unique_ptr<Fl_RGB_Image> pixels(surface.image()); Fl_Surface_Device::pop_current();
    require(pixels && pixels->d() == 3 && pixels->ld() == 0, "Document screenshot failed");
    std::ofstream output(fs::path(MAK3P_DOCUMENT_DIRECTORY).parent_path() / name, std::ios::binary);
    output << "P6\n" << window.w() << ' ' << window.h() << "\n255\n";
    output.write(pixels->data()[0], window.w() * window.h() * 3);
}
template<class Predicate> void click_document_link(Fl_Help_View& body, Predicate followed, int width, int height) {
    // Exercise the renderer's actual link hit boxes, including wrapped text.
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    const int right = body.x() + std::min(width, body.w() - 24);
    const int bottom = body.y() + std::min(height, body.h());
    for (int y = body.y() + 5; y < bottom; y += 3) {
        for (int x = body.x() + 5; x < right; x += 3) {
            Fl::e_x = x; Fl::e_y = y; Fl::event_is_click(1);
            body.handle(FL_PUSH); body.handle(FL_RELEASE);
            if (followed()) return;
        }
    }
    throw std::runtime_error("Rendered document link did not navigate");
}
void rendered_documents() {
    test::TempDirectory tmp;
    const auto app = tmp.path / fs::path(u8"Mak3P 文書 folder"); fs::create_directory(app);
    fs::copy(fs::path(MAK3P_DOCUMENT_DIRECTORY), app / "documents", fs::copy_options::recursive);
    gui::MainWindow window(app, backend()); window.show();
    await([&] { return window.controller().snapshot().phase == GuiPhase::Idle; });
    menu_action(window, "gui_help"); auto* help = shown_window(gui::text("gui_help")); require(help, "Help missing");
    auto* body = dynamic_cast<Fl_Help_View*>(help->child(0)); require(body && body->filename(), "Help did not load HTML");
    require(std::strstr(body->value(), "<h1>Mak3P Help</h1>"), "Help heading missing");
    require(body->scrollbar()->visible() && !body->hscrollbar()->visible(), "Help must flow vertically without horizontal scrolling");
    screenshot(*help, "help-document.ppm");
    body->topline("fnref1"); const auto reference = body->topline();
    click_document_link(*body, [&] { return body->topline() > reference + 500; }, body->w(), 260);
    const auto footnote = body->topline(); screenshot(*help, "help-footnotes.ppm");
    click_document_link(*body, [&] { return body->topline() < footnote - 500; }, body->w(), 260);
    help->hide();
    menu_action(window, "gui_about"); auto* about = shown_window(gui::text("gui_about")); require(about, "About missing");
    button_action(*about, "gui_license"); auto* license = shown_window(gui::text("gui_license")); require(license, "License missing");
    body = dynamic_cast<Fl_Help_View*>(license->child(0)); require(body && body->filename(), "EULA did not load HTML");
    require(std::strstr(body->value(), "End-user license agreement for Mak3P") &&
            std::strstr(body->value(), "Third-party components"), "License document sections missing");
    require(!body->hscrollbar()->visible(), "EULA needs horizontal scrolling"); screenshot(*license, "license-document.ppm");
    for (const auto* name : {"fltk-license", "fltk-utf8-license", "taglib-lgpl", "taglib-mpl", "json-license", "utfcpp-license", "zlib-license"}) {
        const auto path = (app / "documents" / (std::string(name) + ".html")).generic_u8string();
        require(body->load(reinterpret_cast<const char*>(path.c_str())) == 0, "Local component license could not open from Unicode directory");
        require(!body->hscrollbar()->visible(), "Component license needs horizontal scrolling");
        // The component notice places its return link below the heading.
        body->topline(0);
        click_document_link(*body, [&] { return fs::path(body->filename()).filename() == "licenses.html"; }, 160, 100);
        require(body->topline() > 0 && std::strstr(body->value(), "Third-party components"), "Component return link did not reach the EULA section");
    }
    button_action(*license, "gui_close"); require(!license->shown(), "Document Close button failed");
    fs::remove(app / "documents" / "help.html"); menu_action(window, "gui_help");
    help = shown_window(gui::text("gui_help")); body = dynamic_cast<Fl_Help_View*>(help->child(0));
    require(body && std::strstr(body->value(), gui::text("gui_document_failed").c_str()), "Missing document did not show a readable error");
    button_action(*help, "gui_close"); about->hide(); close(window);
}

std::string title(Display* display, Window window) {
    Atom actual; int format; unsigned long count, remaining; unsigned char* data = nullptr;
    const auto property = XInternAtom(display, "_NET_WM_NAME", False);
    std::string result;
    if (XGetWindowProperty(display, window, property, 0, 2048, False, AnyPropertyType,
                           &actual, &format, &count, &remaining, &data) == Success && data) {
        result.assign(reinterpret_cast<char*>(data), count); XFree(data);
    }
    if (result.empty()) { char* name = nullptr; if (XFetchName(display, window, &name) && name) { result = name; XFree(name); } }
    return result;
}
Window find_picker(Display* display, Window parent, const std::string& expected) {
    if (title(display, parent) == expected) {
        Atom actual; int format; unsigned long count, remaining; unsigned char* data = nullptr;
        XGetWindowProperty(display, parent, XInternAtom(display, "_NET_WM_PID", False), 0, 1, False,
                           XA_CARDINAL, &actual, &format, &count, &remaining, &data);
        const bool owned = data && format == 32 && count == 1 && *reinterpret_cast<unsigned long*>(data) == static_cast<unsigned long>(getpid());
        if (data) XFree(data);
        if (owned) return parent;
    }
    Window root, owner, *children = nullptr; unsigned count = 0; Window found = 0;
    if (XQueryTree(display, parent, &root, &owner, &children, &count)) {
        for (unsigned i = 0; i < count && !found; ++i) found = find_picker(display, children[i], expected);
        if (children) XFree(children);
    }
    return found;
}
void close_picker(Display* display, Window picker) {
    XEvent event{}; event.xclient.type = ClientMessage; event.xclient.window = picker;
    event.xclient.message_type = XInternAtom(display, "WM_PROTOCOLS", False); event.xclient.format = 32;
    event.xclient.data.l[0] = XInternAtom(display, "WM_DELETE_WINDOW", False); event.xclient.data.l[1] = CurrentTime;
    XSendEvent(display, picker, False, NoEventMask, &event); XFlush(display);
}
void pickers() {
    test::TempDirectory tmp; gui::MainWindow window(tmp.path, backend()); window.show();
    await([&] { return window.controller().snapshot().phase == GuiPhase::Idle; }); window.refresh();
    const auto filter = gui::text("gui_files_filter", {{"extensions", audio_file_glob()}});
    require(filter.find("*.{flac,ape,wv") != filter.npos, "File filter did not retain literal braces");
    for (bool via_menu : {true, false}) for (const auto* key : {"gui_select_files", "gui_select_folders"}) {
        std::atomic<bool> found = false;
        std::jthread closer([&] {
            auto* display = XOpenDisplay(nullptr); if (!display) return;
            const auto deadline = std::chrono::steady_clock::now() + 8s;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto picker = find_picker(display, DefaultRootWindow(display), gui::text(key));
                if (picker) {
                    std::cerr << "Picker found: " << gui::text(key) << "\n";
                    std::this_thread::sleep_for(500ms);
                    close_picker(display, picker); found = true; break;
                }
                std::this_thread::sleep_for(20ms);
            }
            XCloseDisplay(display);
        });
        std::cerr << "Opening " << (via_menu ? "menu: " : "button: ") << gui::text(key) << "\n";
        if (via_menu) menu_action(window, key);
        else {
            Fl_Menu_Button* add = nullptr;
            for (int i = 0; i < window.children(); ++i) if (auto* widget = dynamic_cast<Fl_Menu_Button*>(window.child(i)); widget && widget->label() && widget->label() == gui::text("gui_add")) add = widget;
            require(add, "Add files button missing"); const auto* action = add->find_item(gui::text(key).c_str()); require(action, "Picker action missing"); action->do_callback(add);
        }
        closer.join(); require(found, "Native picker did not open with its requested title");
        require(window.controller().snapshot().batch.jobs.empty(), "Cancelling a picker changed the queue");
    }
    close(window);
}

void xdnd_message(Display* source, Window owner, Window target, const char* name, long first = 0, long second = 0, long third = 0, long fourth = 0) {
    XEvent event{}; event.xclient.type = ClientMessage; event.xclient.window = target;
    event.xclient.message_type = XInternAtom(source, name, False); event.xclient.format = 32;
    event.xclient.data.l[0] = owner; event.xclient.data.l[1] = first;
    event.xclient.data.l[2] = second; event.xclient.data.l[3] = third; event.xclient.data.l[4] = fourth;
    XSendEvent(source, target, False, NoEventMask, &event); XFlush(source);
}
void native_drop(gui::MainWindow& window, const std::string& payload, std::size_t expected, std::chrono::seconds timeout = 10s) {
    auto* source = XOpenDisplay(nullptr); require(source, "Cannot open source X11 connection");
    const auto owner = XCreateSimpleWindow(source, DefaultRootWindow(source), 0, 0, 10, 10, 0, 0, 0);
    const auto selection = XInternAtom(source, "XdndSelection", False), uri = XInternAtom(source, "text/uri-list", False);
    const auto target = fl_x11_xid(&window); XSetSelectionOwner(source, selection, owner, CurrentTime);
    Fl::belowmouse(nullptr);
    xdnd_message(source, owner, target, "XdndEnter", 5L << 24, uri);
    int x, y; Window child; XTranslateCoordinates(source, target, DefaultRootWindow(source), 30, 80, &x, &y, &child);
    xdnd_message(source, owner, target, "XdndPosition", 0, (static_cast<long>(x) << 16) | (y & 0xffff), CurrentTime, XInternAtom(source, "XdndActionCopy", False));
    try {
        await([&] { return Fl::belowmouse() == &window; }, 2s);
        xdnd_message(source, owner, target, "XdndDrop", 0, CurrentTime);
        await([&] {
            while (XPending(source)) {
                XEvent event; XNextEvent(source, &event);
                if (event.type == SelectionRequest) {
                    const auto& request = event.xselectionrequest;
                    XChangeProperty(source, request.requestor, request.property, request.target, 8, PropModeReplace,
                                    reinterpret_cast<const unsigned char*>(payload.data()), static_cast<int>(payload.size()));
                    XEvent response{}; response.xselection.type = SelectionNotify; response.xselection.display = source;
                    response.xselection.requestor = request.requestor; response.xselection.selection = request.selection;
                    response.xselection.target = request.target; response.xselection.property = request.property; response.xselection.time = request.time;
                    XSendEvent(source, request.requestor, False, NoEventMask, &response); XFlush(source);
                }
            }
            const auto value = window.controller().snapshot();
            return value.phase == GuiPhase::Idle && value.batch.jobs.size() == expected;
        }, timeout);
    } catch (const std::exception& error) {
        const auto value = window.controller().snapshot();
        std::string detail = std::string(error.what()) + ": drop phase=" + std::to_string(static_cast<int>(value.phase)) +
            " rows=" + std::to_string(value.batch.jobs.size());
        for (const auto& notice : value.notices) detail += " " + message(notice) + " " + notice.technical_detail;
        for (const auto& row : value.batch.jobs) detail += " [" + path_utf8(row.job.source().path.filename()) + ": " + message(row.diagnostic) + "]";
        XDestroyWindow(source, owner); XCloseDisplay(source);
        throw std::runtime_error(detail);
    }
    XDestroyWindow(source, owner); XCloseDisplay(source);
}
void drops_and_conversion(const fs::path& fixtures, const EnginePaths& paths) {
    test::TempDirectory tmp;
    fs::copy_file(fixtures / "mono.wav", tmp.path / "selected.wav");
    fs::copy_file(fixtures / "cbr.mp3", tmp.path / "matching.mp3");
    fs::copy_file(fixtures / "invalid.mp3", tmp.path / "invalid.mp3");
    gui::MainWindow window(tmp.path, batch_backend(std::make_shared<AudioProcessor>(paths))); window.show();
    await([&] { return window.controller().snapshot().phase == GuiPhase::Idle; });
    auto settings = window.controller().snapshot().settings; settings.audio.bitrate_kbps = 256;
    require(window.controller().configure(settings), "Drop target setup failed");
    await([&] { return window.controller().snapshot().phase == GuiPhase::Idle && window.controller().snapshot().settings == settings; }); window.refresh();
    std::string payload;
    for (const auto* filename : {"selected.wav", "matching.mp3", "invalid.mp3"}) payload += "file://" + path_utf8(tmp.path / filename) + "\r\n";
    native_drop(window, payload, 3); window.refresh();
    require(window.controller().snapshot().batch.counts.ready == 1 && window.controller().snapshot().batch.counts.skipped == 2, "Dropped audio classification failed");
    button_action(window, "gui_start");
    await([&] { const auto value = window.controller().snapshot(); return value.phase == GuiPhase::Idle && value.batch.counts.completed == 1; });
    require(analyze_mp3(tmp.path / "selected.mp3").bitrate_kbps == 256, "GUI drop/start did not publish valid audio");
    close(window);
}
void folder_drop(const fs::path& source, const EnginePaths& paths) {
    test::TempDirectory tmp;
    const auto& destination = tmp.path;
    fs::create_directories(destination / "outputs");
    AppSettings settings;
    settings.audio.include_mp3_from_folders = true;
    settings.output_mode = OutputMode::CustomDirectory;
    settings.custom_output_directory = destination / "outputs";
    settings.workers = 2;
    settings.logging_enabled = true;
    test::write(destination / "config.ini", serialize_settings(settings));
    auto processor = std::make_shared<AudioProcessor>(paths);
    auto real_backend = batch_backend(processor);
    std::atomic<int> scans = 0;
    const auto inspect = real_backend.inspect_source;
    real_backend.inspect_source = [&](const SourceFile& file, std::stop_token stop) {
        ++scans;
        return inspect(file, stop);
    };
    gui::MainWindow window(destination, std::move(real_backend)); window.show();
    await([&] { return window.controller().snapshot().phase == GuiPhase::Idle; });
    std::string uri = "file://";
    for (char c : path_utf8(source)) uri += c == ' ' ? "%20" : std::string(1, c);
    native_drop(window, uri + "\r\n", 7);
    auto snapshot = window.controller().snapshot();
    require(scans == 10 && snapshot.batch.counts.ready == 6 && snapshot.batch.counts.skipped == 1,
        "Folder drop did not classify six audio files and a disguised PNG");
    window.refresh();
    for (const auto& [rate, channels, ready] : {
        std::tuple{160, ChannelMode::Auto, 3}, {128, ChannelMode::Mono, 6},
        {320, ChannelMode::Stereo, 6}}) {
        settings.audio.bitrate_kbps = rate; settings.audio.channel_mode = channels;
        require(window.controller().configure(settings), "Folder settings rejected");
        await([&] {
            snapshot = window.controller().snapshot();
            return snapshot.phase == GuiPhase::Idle && snapshot.settings == settings &&
                snapshot.batch.counts.ready == static_cast<std::size_t>(ready);
        });
        require(scans == 10, "Changing folder settings rescanned source files");
    }
    for (const bool include : {false, true}) {
        settings.audio.include_mp3_from_folders = include;
        require(window.controller().configure(settings), "Folder MP3 toggle rejected");
        await([&] {
            snapshot = window.controller().snapshot();
            return snapshot.phase == GuiPhase::Idle && snapshot.settings == settings &&
                snapshot.batch.jobs.size() == (include ? 7U : 2U);
        });
        require(scans == 10 && snapshot.batch.counts.excluded == (include ? 0U : 5U),
            "Folder MP3 toggle rescanned sources or left a stale exclusion count");
    }
    button_action(window, "gui_start");
    await([&] {
        snapshot = window.controller().snapshot();
        return snapshot.phase == GuiPhase::Idle && snapshot.has_run;
    }, 20s);
    require(snapshot.last_run.completed == 6 && snapshot.last_run.failed == 0 &&
        snapshot.batch.counts.skipped == 1 && scans == 10 && snapshot.batch.log_path,
        "GUI conversion failed, rescanned sources, or lost its log");
    for (const auto& row : snapshot.batch.jobs) {
        if (row.job.state() == JobState::Completed) {
            require(row.job.output() && analyze_mp3(*row.job.output()).bitrate_kbps == row.output_bitrate_kbps,
                "GUI published an invalid MP3");
        } else require(row.diagnostic.key == "no_audio", "Disguised PNG lost its skip explanation");
    }
    close(window);
}
}
int main(int argc, char** argv) {
    try {
        XInitThreads();
        if (argc != 5) return 2;
        const std::string mode = argv[1];
        const fs::path fixtures = argv[2];
        const EnginePaths paths{argv[3], argv[4]};
        if (mode == "constraints") constraints_and_documents();
        else if (mode == "icons") original_icons();
        else if (mode == "documents") rendered_documents();
        else if (mode == "pickers") pickers();
        else if (mode == "drop") drops_and_conversion(fixtures, paths);
        else if (mode == "folder") folder_drop(fixtures / "Media folder", paths);
        else throw std::runtime_error("Unknown native test mode");
        std::cout << mode << " passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
