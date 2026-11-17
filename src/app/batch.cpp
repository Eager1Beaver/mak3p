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

    void log_error(const std::exception& error, const std::string& key) {
        log.reset();
        std::lock_guard lock(mutex);
        warnings.push_back({key, {}, error.what()});
    }

    void emit(const ConversionJob& job, const Row& row) {
        std::lock_guard lock(log_mutex);
        if (log) {
            try { log->outcome(job, row.diagnostic, row.warnings); }
            catch (const std::exception& error) { log_error(error, "log_write_failed"); }
        }
    }

    void create_log() {
        if (!settings.logging_enabled) return;
        // The queue cannot be mutated while this coordinator owns the run.
        try {
            log = std::make_unique<RunLog>(directory, version, backend.ffmpeg_version,
                                           backend.ffprobe_version, settings, queue.jobs());
            std::lock_guard lock(mutex);
            log_path = log->path();
        } catch (const std::exception& error) { log_error(error, "log_create_failed"); }
    }

    void inspect_one(JobId id) {
        SourceFile source;
        {
            std::lock_guard lock(mutex);
            source = queue.job(id).source();
        }
        Inspection input;
        try { input = backend.inspect(source, settings.audio, stop.get_token()); }
        catch (const std::exception& error) {
            input.source = source;
            input.settings = settings.audio;
            input.diagnostic = {"unsupported", {}, error.what()};
        }
        std::optional<ConversionJob> event;
        Row event_row;
        {
            std::lock_guard lock(mutex);
            auto& job = queue.job(id);
            auto& row = rows.at(id);
            row.inspection = std::move(input);
            auto& inspection = *row.inspection;
            row.diagnostic = inspection.diagnostic;
            row.warnings = inspection.warnings;
            if (stop.stop_requested() || inspection.state == JobState::Cancelled) {
                job.cancel(); row.diagnostic = {"cancelled", {}, {}};
            } else if (inspection.state == JobState::Ready) job.mark_ready();
            else if (inspection.state == JobState::SameBitrateSkipped) {
                job.skip_same_bitrate(message(row.diagnostic));
            } else if (inspection.state == JobState::LowerBitrateSkipped) {
                job.skip_lower_bitrate(message(row.diagnostic));
            } else job.skip_unsupported(message(row.diagnostic));
            if (job.state() != JobState::Ready) { event = job; event_row = row; }
            if (row.diagnostic.key == "mp3_excluded" && job.source().origin == ImportOrigin::FolderScan) {
                (void)queue.remove(id); rows.erase(id); ++excluded;
            }
        }
        if (event) emit(*event, event_row);
    }

    void finish_conversion(JobId id, ConversionResult result) {
        std::optional<ConversionJob> event;
        Row event_row;
        {
            std::lock_guard lock(mutex);
            auto& job = queue.job(id);
            auto& row = rows.at(id);
            row.diagnostic = std::move(result.diagnostic);
            row.warnings = std::move(result.warnings);
            row.finished = Clock::now();
            if (result.state == JobState::Completed && result.output) job.complete(*result.output);
            else if (result.state == JobState::Cancelled) job.cancel();
            else {
                if (row.diagnostic.key.empty()) row.diagnostic = {"conversion_failed", {}, {}};
                job.fail(message(row.diagnostic));
            }
            event = job; event_row = row;
        }
        emit(*event, event_row);
    }

    template<class Work>
    void parallel(const std::vector<JobId>& ids, Work work) {
        const auto count = worker_count(settings.workers, std::thread::hardware_concurrency(), ids.size());
        {
            std::lock_guard lock(mutex);
            workers = count;
        }
        std::size_t next = 0;
        std::mutex dispatch;
        std::vector<std::jthread> pool;
        pool.reserve(count);
        try {
            for (std::size_t index = 0; index < count; ++index) {
                pool.emplace_back([&] {
                    for (;;) {
                        JobId id;
                        {
                            std::lock_guard lock(dispatch);
                            if (stop.stop_requested() || next == ids.size()) return;
                            id = ids[next++];
                        }
                        try { work(id); }
                        catch (const std::exception& error) {
                            // Unexpected backend/controller errors are contained in this file.
                            std::optional<ConversionJob> event;
                            Row event_row;
                            {
                                std::lock_guard lock(mutex);
                                auto& job = queue.job(id);
                                auto& row = rows.at(id);
                                row.diagnostic = {"conversion_failed", {}, error.what()};
                                if (job.state() == JobState::Checking) job.skip_unsupported(message(row.diagnostic));
                                else if (job.state() == JobState::Ready) {
                                    job.start(); job.fail(message(row.diagnostic));
                                } else if (job.state() == JobState::Converting) job.fail(message(row.diagnostic));
                                row.finished = Clock::now();
                                event = job; event_row = row;
                            }
                            emit(*event, event_row);
                        }
                    }
                });
            }
        } catch (...) {
            stop.request_stop();
            for (auto& worker : pool) worker.join();
            throw;
        }
        // Join explicitly: jthread destruction requests its own unrelated stop token.
        for (auto& worker : pool) worker.join();
    }

    void run(bool convert) {
        try {
            if (convert) create_log();
            std::vector<JobId> ids;
            {
                std::lock_guard lock(mutex);
                for (const auto& job : queue.jobs()) ids.push_back(job.id());
            }
            parallel(ids, [&](JobId id) { inspect_one(id); });
            if (convert && !stop.stop_requested()) {
                ids.clear();
                {
                    std::lock_guard lock(mutex);
                    phase = BatchPhase::Converting;
                    for (const auto& job : queue.jobs()) if (job.state() == JobState::Ready) ids.push_back(job.id());
                }
                OutputPathPlanner planner;
                std::mutex allocation;
                parallel(ids, [&](JobId id) {
                    std::unique_ptr<OutputTransaction> transaction;
                    Inspection input;
                    {
                        std::lock_guard allocation_lock(allocation);
                        {
                            std::lock_guard lock(mutex);
                            if (stop.stop_requested()) return;
                            auto& job = queue.job(id);
                            job.start();
                            auto& row = rows.at(id);
                            row.started = Clock::now();
                            row.diagnostic = {"converting", {}, {}};
                            input = *row.inspection;
                        }
                        const auto destination = settings.output_mode == OutputMode::CustomDirectory ?
                            std::optional<fs::path>(settings.custom_output_directory) : std::nullopt;
                        // The allocator reserves the name before releasing a job to conversion.
                        transaction = std::make_unique<OutputTransaction>(planner,
                            OutputRequest{input.source.path, destination});
                    }
                    ConversionResult result;
                    try {
                        result = backend.convert(input, settings.audio, *transaction, stop.get_token(),
                            [this, id](const ProgressUpdate& update) {
                                std::lock_guard lock(mutex);
                                auto& job = queue.job(id);
                                if (job.state() != JobState::Converting) return;
                                auto fraction = update.fraction;
                                if (fraction) *fraction = std::min(*fraction, 0.99);
                                if (!fraction || !job.progress() || *fraction >= *job.progress()) job.update_progress(fraction);
                            });
                    } catch (const std::exception& error) {
                        result.diagnostic = {"conversion_failed", {}, error.what()};
                    }
                    // A backend may throw after publishing; publication is irreversible.
                    if (transaction->committed()) {
                        result.state = JobState::Completed;
                        result.output = transaction->final_path();
                        result.diagnostic = {"completed", {}, {}};
                    } else {
                        if (stop.stop_requested()) {
                            result.state = JobState::Cancelled;
                            result.diagnostic = {"cancelled", {}, {}};
                        } else if (result.state == JobState::Completed) {
                            result.state = JobState::Failed;
                            result.diagnostic = {"output_invalid", {}, "The output was not committed"};
                        }
                        try { transaction->discard(); }
                        catch (const std::exception& error) { result.warnings.push_back({"cleanup_failed", {}, error.what()}); }
                    }
                    finish_conversion(id, std::move(result));
                });
            }
        } catch (const std::exception& error) {
            {
                std::lock_guard lock(mutex);
                warnings.push_back({"batch_failed", {}, error.what()});
            }
            stop.request_stop();
        }
        std::vector<std::pair<ConversionJob, Row>> unfinished;
        BatchCounts counts;
        {
            std::lock_guard lock(mutex);
            for (const auto& entry : queue.jobs()) {
                auto& job = queue.job(entry.id());
                if (stop.stop_requested() && (job.state() == JobState::Checking ||
                    job.state() == JobState::Ready || job.state() == JobState::Converting)) {
                    job.cancel();
                    auto& row = rows.at(job.id());
                    row.diagnostic = {"cancelled", {}, {}};
                    if (row.started) row.finished = Clock::now();
                    unfinished.emplace_back(job, row);
                }
            }
            counts = counts_for(queue.jobs(), excluded);
        }
        for (const auto& [job, row] : unfinished) emit(job, row);
        if (log) {
            try { log->finish(counts.completed, counts.skipped, counts.failed, counts.cancelled, counts.excluded); }
            catch (const std::exception& error) { log_error(error, "log_write_failed"); }
        }
        log.reset();
        std::lock_guard lock(mutex);
        workers = 0;
        phase = BatchPhase::Idle;
    }

    void launch(bool convert) {
        std::lock_guard lifecycle_lock(lifecycle);
        {
            std::lock_guard lock(mutex);
            require_idle();
        }
        if (coordinator.joinable()) coordinator.join();
        std::lock_guard lock(mutex);
        for (const auto& entry : queue.jobs()) {
            queue.job(entry.id()).reset();
            rows.at(entry.id()) = Row{};
        }
        warnings.clear(); log_path.reset(); excluded = 0; workers = 0;
        stop = std::stop_source{};
        phase = BatchPhase::Inspecting;
        try { coordinator = std::jthread([this, convert] { run(convert); }); }
        catch (...) { phase = BatchPhase::Idle; throw; }
    }
};

BatchEngine::BatchEngine(BatchBackend backend, fs::path directory, std::string version)
    : impl_(std::make_unique<Impl>()) {
    if (!backend.inspect || !backend.convert) throw std::invalid_argument("Both audio operations are required");
    impl_->backend = std::move(backend);
    impl_->directory = fs::absolute(directory).lexically_normal();
    impl_->version = std::move(version);
}

BatchEngine::~BatchEngine() { cancel(); wait(); }

QueueAddResult BatchEngine::add(const fs::path& path, ImportOrigin origin) {
    std::lock_guard lock(impl_->mutex);
    impl_->require_idle();
    const auto result = impl_->queue.add(path, origin);
    if (result.added) impl_->rows.emplace(result.id, Impl::Row{});
    else if (result.origin_changed) {
        impl_->queue.job(result.id).reset(); impl_->rows.at(result.id) = Impl::Row{};
    }
    return result;
}


} // namespace audio_converter
