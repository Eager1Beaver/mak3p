#include "gui/main_window.hpp"
#include "app/identity.hpp"
#include <FL/Fl.H>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--version") {
        std::cout << audio_converter::application_name << ' ' << audio_converter::application_version << '\n';
        return 0;
    }
    Fl::scheme("gtk+");
#if defined(MAK3P_WINDOWS_FULL) || defined(MAK3P_LINUX_FULL)
    constexpr auto engine_mode = audio_converter::EngineMode::Bundled;
#else
    constexpr auto engine_mode = audio_converter::EngineMode::Discover;
#endif
    audio_converter::gui::MainWindow window(audio_converter::executable_directory(), std::nullopt, engine_mode);
    window.show();
    return Fl::run();
}
