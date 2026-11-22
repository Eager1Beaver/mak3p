#include "app/batch.hpp"
#include "app/run_log.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <set>

namespace audio_converter {
namespace {
using Clock = std::chrono::steady_clock;

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
        }, processor->ffmpeg_version(), processor->ffprobe_version(),
        [processor](const SourceFile& source, std::stop_token stop) {
            return processor->inspect_source(source, stop);
        }
    };
}

struct BatchEngine::Impl {
    struct Row {
        std::shared_ptr<const SourceInspection> source_inspection;
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
    std::set<JobId> active;
    bool incremental = false;

    void reset_row(JobId id) {
        auto& row = rows.at(id);
        auto cached = std::move(row.source_inspection);
        row = Row{};
        row.source_inspection = std::move(cached);
    }

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
            try { log->outcome(job, row.diagnostic, row.warnings,
                               row.inspection ? row.inspection->output_bitrate_kbps : std::nullopt); }
            catch (const std::exception& error) { log_error(error, "log_write_failed"); }
        }
    }

    std::vector<ConversionJob> log_inputs() const {
        std::vector<ConversionJob> inputs;
        for (const auto& job : queue.jobs()) {
            if (!incremental || active.contains(job.id()) ||
                job.state() != JobState::Completed) inputs.push_back(job);
        }
        return inputs;
    }

    void create_log() {
        if (!settings.logging_enabled) return;
        // The queue cannot be mutated while this coordinator owns the run.
        try {
            log = std::make_unique<RunLog>(directory, version, backend.ffmpeg_version,
                                           backend.ffprobe_version, settings, log_inputs());
            std::lock_guard lock(mutex);
            log_path = log->path();
        } catch (const std::exception& error) { log_error(error, "log_create_failed"); }
    }

    void inspect_one(JobId id) {
        SourceFile source;
        std::shared_ptr<const SourceInspection> cached;
        {
            std::lock_guard lock(mutex);
            source = queue.job(id).source();
            cached = rows.at(id).source_inspection;
        }
        Inspection input;
        try {
            if (backend.inspect_source) {
                if (!cached) cached = std::make_shared<const SourceInspection>(
                    backend.inspect_source(source, stop.get_token()));
                input = classify_source(*cached, settings.audio);
            } else input = backend.inspect(source, settings.audio, stop.get_token());
        }
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
            if (cached && cached->state != JobState::Cancelled) row.source_inspection = std::move(cached);
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
            if (convert) {
                create_log();
                if (incremental) {
                    for (const auto& job : log_inputs())
                        if (!active.contains(job.id())) emit(job, rows.at(job.id()));
                }
            }
            std::vector<JobId> ids;
            {
                std::lock_guard lock(mutex);
                for (const auto& job : queue.jobs()) if (active.contains(job.id())) ids.push_back(job.id());
            }
            parallel(ids, [&](JobId id) { inspect_one(id); });
            if (convert && !stop.stop_requested()) {
                ids.clear();
                {
                    std::lock_guard lock(mutex);
                    phase = BatchPhase::Converting;
                    for (const auto& job : queue.jobs()) if (active.contains(job.id()) && job.state() == JobState::Ready) ids.push_back(job.id());
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
                if (active.contains(job.id()) && stop.stop_requested() && (job.state() == JobState::Checking ||
                    job.state() == JobState::Ready || job.state() == JobState::Converting)) {
                    job.cancel();
                    auto& row = rows.at(job.id());
                    row.diagnostic = {"cancelled", {}, {}};
                    if (row.started) row.finished = Clock::now();
                    unfinished.emplace_back(job, row);
                }
            }
            const auto inputs = log_inputs();
            counts = counts_for(inputs, excluded);
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

    void launch(bool convert, bool pending = false) {
        std::lock_guard lifecycle_lock(lifecycle);
        {
            std::lock_guard lock(mutex);
            require_idle();
        }
        if (coordinator.joinable()) coordinator.join();
        std::lock_guard lock(mutex);
        active.clear(); incremental = pending;
        for (const auto& entry : queue.jobs()) {
            if (pending && entry.state() != JobState::Checking &&
                !(entry.state() == JobState::Cancelled || (convert && entry.state() == JobState::Ready))) continue;
            active.insert(entry.id());
            queue.job(entry.id()).reset();
            reset_row(entry.id());
        }
        warnings.clear(); log_path.reset(); if (!pending) excluded = 0; workers = 0;
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
        impl_->queue.job(result.id).reset(); impl_->reset_row(result.id);
        auto& cached = impl_->rows.at(result.id).source_inspection;
        if (cached) {
            auto promoted = std::make_shared<SourceInspection>(*cached);
            promoted->source.origin = origin;
            cached = std::move(promoted);
        }
    }
    return result;
}

bool BatchEngine::remove(JobId id) {
    std::lock_guard lock(impl_->mutex);
    impl_->require_idle();
    if (!impl_->queue.remove(id)) return false;
    impl_->rows.erase(id);
    return true;
}

void BatchEngine::clear() {
    std::lock_guard lock(impl_->mutex);
    impl_->require_idle();
    impl_->queue.clear(); impl_->rows.clear(); impl_->excluded = 0;
}

void BatchEngine::configure(AppSettings settings) {
    validate_settings(settings);
    if (!settings.custom_output_directory.empty()) {
        settings.custom_output_directory = fs::absolute(settings.custom_output_directory).lexically_normal();
    }
    std::lock_guard lock(impl_->mutex);
    impl_->require_idle();
    if (impl_->settings == settings) return;
    const bool invalidate = impl_->settings.audio.bitrate_kbps != settings.audio.bitrate_kbps ||
        impl_->settings.audio.channel_mode != settings.audio.channel_mode ||
        impl_->settings.audio.force_upsampling != settings.audio.force_upsampling ||
        impl_->settings.output_mode != settings.output_mode ||
        impl_->settings.custom_output_directory != settings.custom_output_directory;
    const bool import_changed = impl_->settings.audio.include_mp3_from_folders != settings.audio.include_mp3_from_folders;
    impl_->settings = std::move(settings);
    if (!invalidate && !import_changed) return;
    for (const auto& entry : impl_->queue.jobs()) {
        if (!invalidate && entry.state() == JobState::Completed) continue;
        impl_->queue.job(entry.id()).reset(); impl_->reset_row(entry.id());
    }
}

AppSettings BatchEngine::settings() const {
    std::lock_guard lock(impl_->mutex); return impl_->settings;
}
void BatchEngine::inspect_async() { impl_->launch(false); }
void BatchEngine::start_async() { impl_->launch(true); }
void BatchEngine::inspect_pending_async() { impl_->launch(false, true); }
void BatchEngine::start_pending_async() { impl_->launch(true, true); }
void BatchEngine::cancel() {
    std::stop_source source;
    { std::lock_guard lock(impl_->mutex); source = impl_->stop; }
    source.request_stop();
}
void BatchEngine::wait() {
    std::lock_guard lock(impl_->lifecycle);
    if (impl_->coordinator.joinable()) impl_->coordinator.join();
}

BatchSnapshot BatchEngine::snapshot() const {
    std::lock_guard lock(impl_->mutex);
    BatchSnapshot result;
    result.phase = impl_->phase; result.workers = impl_->workers;
    result.warnings = impl_->warnings; result.log_path = impl_->log_path;
    result.counts = counts_for(impl_->queue.jobs(), impl_->excluded);
    const auto now = Clock::now();
    for (const auto& job : impl_->queue.jobs()) {
        const auto& row = impl_->rows.at(job.id());
        JobSnapshot item{job, row.diagnostic, row.warnings, std::nullopt, row.started.has_value(), 0, std::nullopt};
        if (row.inspection) {
            item.media = row.inspection->media;
            if (row.inspection->diagnostic.key != "source_unreadable") item.source_size = row.inspection->source_size;
            item.output_bitrate_kbps = row.inspection->output_bitrate_kbps;
        }
        if (row.started) {
            item.elapsed_seconds = std::chrono::duration<double>(row.finished.value_or(now) - *row.started).count();
            if (job.state() == JobState::Completed) item.eta_seconds = 0;
            else if (job.state() == JobState::Converting && job.progress() && *job.progress() > 0 &&
                     item.elapsed_seconds >= 0.05) {
                const auto seconds = item.elapsed_seconds * (1.0 - *job.progress()) / *job.progress();
                if (std::isfinite(seconds)) item.eta_seconds = seconds;
            }
        }
        result.jobs.push_back(std::move(item));
    }
    return result;
}

} // namespace audio_converter
