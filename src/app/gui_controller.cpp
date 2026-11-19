#include "app/gui_controller.hpp"
#include "app/import.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace audio_converter {
GuiController::GuiController(fs::path directory, std::optional<BatchBackend> backend)
    : worker_([this, directory = std::move(directory), backend = std::move(backend)](std::stop_token stop) mutable {
        run(std::move(directory), std::move(backend), stop);
    }) {}
GuiController::~GuiController() { close(); }
GuiSnapshot GuiController::snapshot() const { std::lock_guard lock(mutex_); return state_; }
bool GuiController::submit(Command command, bool unrestricted) {
    std::lock_guard lock(mutex_);
    if (closing_ || (!unrestricted && (state_.phase != GuiPhase::Idle || std::any_of(commands_.begin(), commands_.end(), [](const auto& value) { return value.operation != Operation::Window; })))) return false;
    commands_.push_back(std::move(command)); condition_.notify_one(); return true;
}
bool GuiController::add(std::vector<fs::path> paths) { return submit({Operation::Add, std::move(paths)}); }
bool GuiController::remove(JobId id) { return submit({Operation::Remove, {}, id}); }
bool GuiController::clear() { return submit({Operation::Clear, {}}); }
bool GuiController::configure(AppSettings settings) { return submit({Operation::Configure, {}, 0, std::move(settings)}); }
bool GuiController::start() {
    if (!snapshot().can_start) return false;
    return submit({Operation::Start, {}});
}
void GuiController::window_size(int width, int height) {
    Command command{Operation::Window, {}}; command.settings.window = {std::clamp(width, gui::metrics::minimum_width, 16384), std::clamp(height, gui::metrics::minimum_height, 16384)};
    std::lock_guard lock(mutex_);
    if (closing_) return;
    std::erase_if(commands_, [](const auto& item) { return item.operation == Operation::Window; });
    commands_.push_back(std::move(command)); condition_.notify_one();
}
void GuiController::select_engine(fs::path directory) { (void)submit({Operation::Engine, {std::move(directory)}}, true); }
void GuiController::cancel() { cancel_ = true; condition_.notify_one(); }
void GuiController::close() { closing_ = true; worker_.request_stop(); condition_.notify_one(); }

} // namespace audio_converter
