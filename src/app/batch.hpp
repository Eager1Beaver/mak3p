#pragma once

#include "app/settings.hpp"
#include "app/identity.hpp"

#include <memory>
#include <mutex>
#include <thread>

namespace audio_converter {

struct BatchBackend {
    // Operations may run concurrently and must finish their writers before returning.
    std::function<Inspection(const SourceFile&, const AudioSettings&, std::stop_token)> inspect;
    std::function<ConversionResult(const Inspection&, 
                                   const AudioSettings&, 
                                   OutputTransaction&,
                                   std::stop_token, 
                                   ProgressCallback)> convert;
    std::string ffmpeg_version;
    std::string ffprobe_version;
};

[[nodiscard]] BatchBackend batch_backend(std::shared_ptr<const AudioProcessor> processor);

enum class BatchPhase { Idle, Inspecting, Converting };

struct BatchCounts {
    std::size_t total = 0;
    std::size_t checking = 0;
    std::size_t ready = 0;
    std::size_t converting = 0;
    std::size_t completed = 0;
    std::size_t skipped = 0;
    std::size_t failed = 0;
    std::size_t cancelled = 0;
    std::size_t excluded = 0;
};

struct JobSnapshot {
    ConversionJob job;
    Diagnostic diagnostic;
    std::vector<Diagnostic> warnings;
    std::optional<MediaInfo> media;
    bool progress_visible = false;
    double elapsed_seconds = 0;
    std::optional<double> eta_seconds;
    std::optional<int> output_bitrate_kbps = std::nullopt;
    std::optional<std::uintmax_t> source_size = std::nullopt;
};

struct BatchSnapshot {
    BatchPhase phase = BatchPhase::Idle;
    std::vector<JobSnapshot> jobs;
    BatchCounts counts;
    std::size_t workers = 0;
    std::vector<Diagnostic> warnings;
    std::optional<fs::path> log_path;
};

// Mutations require an idle controller. Snapshots and cancellation are nonblocking
// with respect to audio work; wait() and destruction join all owned workers.
class BatchEngine {
public:
    BatchEngine(BatchBackend backend, 
                fs::path application_directory,
                std::string version = application_version);
    ~BatchEngine();
    BatchEngine(const BatchEngine&) = delete;
    BatchEngine& operator=(const BatchEngine&) = delete;
    [[nodiscard]] QueueAddResult add(const fs::path& path, ImportOrigin origin = ImportOrigin::SelectedFile);
    [[nodiscard]] bool remove(JobId id);
    void clear();
    void configure(AppSettings settings);
    [[nodiscard]] AppSettings settings() const;
    void inspect_async();
    void start_async();
    // Preserve completed history; inspect only invalid rows and run only pending rows.
    void inspect_pending_async();
    void start_pending_async();
    void cancel();
    void wait();
    [[nodiscard]] BatchSnapshot snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace audio_converter
