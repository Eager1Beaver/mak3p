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
    audio_converter::gui::MainWindow window(audio_converter::executable_directory());
    window.show();
    return Fl::run();
}
