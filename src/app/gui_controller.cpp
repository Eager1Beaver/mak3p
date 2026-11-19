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

void GuiController::run(fs::path directory, std::optional<BatchBackend> backend, std::stop_token stop) {
    SettingsStore store(directory);
    AppSettings settings;
    std::unique_ptr<BatchEngine> engine;
    bool converting = false;
    auto phase = [&](GuiPhase value) { std::lock_guard lock(mutex_); state_.phase = value; state_.can_start = false; };
    auto notice = [&](Diagnostic value) {
        std::lock_guard lock(mutex_);
        if (state_.notices.size() == 16) state_.notices.erase(state_.notices.begin());
        state_.notices.push_back(std::move(value));
    };
    auto persist = [&] { for (auto& warning : store.save(settings)) notice(std::move(warning)); };
    auto publish = [&] {
        auto batch = engine ? engine->snapshot() : BatchSnapshot{};
        std::lock_guard lock(mutex_);
        state_.batch = std::move(batch); state_.settings = settings;
        state_.can_start = state_.phase == GuiPhase::Idle && (state_.batch.counts.ready > 0 || state_.batch.counts.cancelled > 0);
    };
    auto initialize = [&] {
        phase(GuiPhase::Initializing);
        try {
            auto processor = [&] {
                const auto paths = discover_engine(directory, settings.engine_directory);
                try { return std::make_shared<AudioProcessor>(paths, stop); }
                catch (const AudioError&) {
                    if (settings.engine_directory.empty() || stop.stop_requested()) throw;
                    // An explicitly selected installation can recover from a broken bundled engine.
                    return std::make_shared<AudioProcessor>(EnginePaths{
                        settings.engine_directory / paths.ffmpeg.filename(),
                        settings.engine_directory / paths.ffprobe.filename()}, stop);
                }
            };
            auto work = backend ? *backend : batch_backend(processor());
            engine = std::make_unique<BatchEngine>(std::move(work), directory);
            engine->configure(settings);
            {
                std::lock_guard lock(mutex_);
                std::erase_if(state_.notices, [](const auto& item) {
                    return item.key.starts_with("engine_") || item.key == "missing_encoder" || item.key == "process_timeout";
                });
            }
            phase(GuiPhase::Idle);
        } catch (const AudioError& error) { notice(error.diagnostic()); phase(GuiPhase::EngineMissing); }
        publish();
    };
    try {
        auto loaded = store.load(); settings = loaded.settings;
        for (auto& warning : loaded.warnings) notice(std::move(warning));
        const auto cpu = std::thread::hardware_concurrency();
        if (settings.workers && cpu && settings.workers > cpu) {
            settings.workers = cpu; notice({"gui_workers_clamped", {{"count", std::to_string(cpu)}}, {}});
        }
        initialize();
        while (!stop.stop_requested()) {
            if (cancel_.exchange(false) && engine && engine->snapshot().phase != BatchPhase::Idle) {
                phase(GuiPhase::Cancelling); engine->cancel();
            }
            if (engine && engine->snapshot().phase != BatchPhase::Idle) {
                publish();
                std::unique_lock lock(mutex_); condition_.wait_for(lock, std::chrono::milliseconds(30)); continue;
            }
            if (engine) {
                auto current = snapshot().phase;
                if (current == GuiPhase::Running || current == GuiPhase::Checking || current == GuiPhase::Cancelling) {
                    engine->wait();
                    for (auto& warning : engine->snapshot().warnings) notice(std::move(warning));
                    if (converting) {
                        const auto result = engine->snapshot();
                        std::lock_guard lock(mutex_);
                        state_.last_run = {};
                        state_.last_run.skipped = result.counts.skipped;
                        for (const auto& row : result.jobs) {
                            if (std::find(state_.run_ids.begin(), state_.run_ids.end(), row.job.id()) == state_.run_ids.end()) continue;
                            ++state_.last_run.total;
                            if (row.job.state() == JobState::Completed) ++state_.last_run.completed;
                            if (row.job.state() == JobState::Failed) ++state_.last_run.failed;
                            if (row.job.state() == JobState::Cancelled) ++state_.last_run.cancelled;
                        }
                    }
                    converting = false; phase(GuiPhase::Idle); publish();
                }
            }
            std::optional<Command> command;
            {
                std::unique_lock lock(mutex_);
                if (commands_.empty()) condition_.wait_for(lock, std::chrono::milliseconds(30));
                if (!commands_.empty()) { command = std::move(commands_.front()); commands_.pop_front(); }
            }
            if (!command || stop.stop_requested()) continue;
            try {
                auto& item = *command;
                switch (item.operation) {
                case Operation::Add: {
                    if (!engine) break;
                    phase(GuiPhase::Importing); publish();
                    auto discovered = discover_sources(item.paths, settings.recursive_folders, stop);
                    for (auto& warning : discovered.warnings) notice(std::move(warning));
                    for (const auto& source : discovered.files) {
                        if (stop.stop_requested()) break;
                        try { (void)engine->add(source.path, source.origin); }
                        catch (const std::exception& error) { notice({"gui_import_failed", {}, error.what()}); }
                    }
                    engine->inspect_pending_async(); phase(GuiPhase::Checking); break;
                }
                case Operation::Remove: if (engine) (void)engine->remove(item.id); break;
                case Operation::Clear:
                    if (engine) engine->clear();
                    { std::lock_guard lock(mutex_); state_.has_run = false; state_.run_ids.clear(); }
                    break;
                case Operation::Configure: {
                    validate_settings(item.settings);
                    const auto window = settings.window;
                    settings = item.settings; settings.window = window;
                    if (engine) {
                        engine->configure(settings);
                        if (engine->snapshot().counts.checking) { engine->inspect_pending_async(); phase(GuiPhase::Checking); }
                    }
                    persist(); break;
                }
                case Operation::Window:
                    settings.window = item.settings.window;
                    if (engine) engine->configure(settings);
                    persist(); break;
                case Operation::Engine:
                    if (engine) break;
                    settings.engine_directory = item.paths.empty() ? fs::path{} : item.paths.front();
                    initialize(); if (engine) persist(); break;
                case Operation::Start:
                    if (!engine || (!engine->snapshot().counts.ready && !engine->snapshot().counts.cancelled)) break;
                    {
                        std::lock_guard lock(mutex_); state_.run_ids.clear();
                        for (const auto& row : engine->snapshot().jobs)
                            if (row.job.state() == JobState::Ready || row.job.state() == JobState::Cancelled) state_.run_ids.push_back(row.job.id());
                        state_.has_run = true;
                    }
                    converting = true; engine->start_pending_async(); phase(GuiPhase::Running); break;
                }
            } catch (const AudioError& error) { notice(error.diagnostic()); phase(engine ? GuiPhase::Idle : GuiPhase::EngineMissing); }
            catch (const std::exception& error) { notice({"gui_operation_failed", {}, error.what()}); phase(engine ? GuiPhase::Idle : GuiPhase::EngineMissing); }
            publish();
        }
        phase(GuiPhase::Closing);
        if (engine) { engine->cancel(); engine->wait(); }
        // Apply the last queued dimensions before the final serialized save.
        { std::lock_guard lock(mutex_); for (const auto& item : commands_)
            if (item.operation == Operation::Window) settings.window = item.settings.window; }
        persist(); publish();
    } catch (const std::exception& error) { notice({"gui_operation_failed", {}, error.what()}); }
    phase(GuiPhase::Closed);
}

} // namespace audio_converter
