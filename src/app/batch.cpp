#include "app/batch.hpp"
#include "app/run_log.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace audio_converter {
namespace {

BatchCounts counts_for(std::span<const ConversionJob> jobs, std::size_t excluded) {
    BatchCounts counts;
    counts.total = jobs.size();
    counts.excluded = excluded;
    for (const auto& job : jobs) {
        switch (job.state()) {
        case JobState::Checking: ++counts.checking; break;
        case JobState::Ready: ++counts.ready; break;
        case JobState::Converting: ++counts.converting; break;
        case JobState::Completed: ++counts.completed; break;
        case JobState::SameBitrateSkipped:
        case JobState::LowerBitrateSkipped:
        case JobState::UnsupportedSkipped: ++counts.skipped; break;
        case JobState::Failed: ++counts.failed; break;
        case JobState::Cancelled: ++counts.cancelled; break;
        }
    }
    return counts;
}
} // namespace

BatchBackend batch_backend(std::shared_ptr<const AudioProcessor> processor) {
    if (!processor) throw std::invalid_argument("An audio processor is required");
    return {
        [processor](const SourceFile& source, const AudioSettings& settings, std::stop_token stop) {
            return processor->inspect(source, settings, stop);
        },
        [processor](const Inspection& input, const AudioSettings& settings, OutputTransaction& output,
                    std::stop_token stop, ProgressCallback progress) {
            return processor->convert(input, settings, output, stop, std::move(progress));
        }, processor->ffmpeg_version(), processor->ffprobe_version()
    };
}

struct BatchEngine::Impl {
    struct Row {
        std::optional<Inspection> inspection;
        Diagnostic diagnostic{"checking", {}, {}};
        std::vector<Diagnostic> warnings;
        std::optional<Clock::time_point> started;
        std::optional<Clock::time_point> finished;
    };
    BatchBackend backend;
    fs::path directory;
    std::string version;
    mutable std::mutex mutex;
    std::mutex lifecycle;
    std::mutex log_mutex;
    FileQueue queue;
    std::map<JobId, Row> rows;
    AppSettings settings;
    BatchPhase phase = BatchPhase::Idle;
    std::size_t workers = 0;
    std::size_t excluded = 0;
    std::vector<Diagnostic> warnings;
    std::optional<fs::path> log_path;
    std::unique_ptr<RunLog> log;
    std::stop_source stop;
    std::jthread coordinator;

    void require_idle() const {
        if (phase != BatchPhase::Idle) throw AudioError({"batch_busy", {}, {}});
    }

} // namespace audio_converter
