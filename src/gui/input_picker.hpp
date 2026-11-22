#pragma once

#include "core/path_utils.hpp"

#include <vector>

class Fl_Window;

namespace audio_converter::gui {

[[nodiscard]] std::vector<fs::path> select_input_paths(Fl_Window& owner, bool folders);

} // namespace audio_converter::gui
