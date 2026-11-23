#pragma once

#include "app/batch.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>

namespace audio_converter {
enum class GuiPhase { Initializing, Idle, Importing, Checking, Running, Cancelling, EngineMissing, Closing, Closed };
enum class EngineMode { Discover, Bundled };
struct GuiSnapshot {
    GuiPhase phase = GuiPhase::Initializing;
    AppSettings settings;
    BatchSnapshot batch;
    std::vector<Diagnostic> notices;
    std::vector<JobId> run_ids;
    BatchCounts last_run;
    bool has_run = false;
    bool can_start = false;
};

// Owns background application work. The presentation reads snapshots and submits commands.
class GuiController {
public:
    explicit GuiController(fs::path directory, std::optional<BatchBackend> backend = std::nullopt,
                           EngineMode engine_mode = EngineMode::Discover);
    ~GuiController();
    GuiController(const GuiController&) = delete;
    GuiController& operator=(const GuiController&) = delete;
    [[nodiscard]] GuiSnapshot snapshot() const;
    bool add(std::vector<fs::path> paths);
    bool remove(JobId id);
    bool clear();
    bool configure(AppSettings settings);
    void window_size(int width, int height);
    void select_engine(fs::path directory);
    bool start();
    void cancel();
    void close();
private:
    enum class Operation { Add, Remove, Clear, Configure, Start, Window, Engine };
    struct Command {
        Operation operation;
        std::vector<fs::path> paths;
        JobId id = 0;
        AppSettings settings{};
    };
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    GuiSnapshot state_;
    std::deque<Command> commands_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> closing_{false};
    std::jthread worker_;
    bool submit(Command command, bool unrestricted = false);
    void run(fs::path directory, std::optional<BatchBackend> backend, EngineMode engine_mode, std::stop_token stop);
};
} // namespace audio_converter
