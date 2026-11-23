#include "app/batch.hpp"
#include "app/run_log.hpp"
#include "audio/process.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <set>
#ifndef _WIN32
#include <sys/resource.h>
#endif

using namespace audio_converter;
using namespace std::chrono_literals;
using test::require;
using test::throws;
using test::TempDirectory;

namespace {
fs::path helper;

template<class Predicate>
void await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Timed out waiting for work");
        std::this_thread::sleep_for(5ms);
    }
}

ConversionResult success(OutputTransaction& output) {
    test::write(output.temporary_path(), "encoded audio");
    return {JobState::Completed, {"completed", {}, {}}, output.commit(), {}};
}

BatchBackend backend() {
    return {
        [](const SourceFile& source, const AudioSettings& settings, std::stop_token) {
            Inspection result;
            result.source = source; result.settings = settings;
            result.state = JobState::Ready; result.diagnostic = {"ready", {}, {}};
            return result;
        },
        [](const Inspection&, const AudioSettings&, OutputTransaction& output,
           std::stop_token, ProgressCallback) { return success(output); },
        "ffmpeg version fixture", "ffprobe version fixture"
    };
}

void no_partials(const fs::path& directory) {
    for (const auto& item : fs::directory_iterator(directory)) {
        require(item.path().filename().string().find("partial.mp3") == std::string::npos,
                "A temporary MP3 survived the batch");
    }
}

void worker_policy() {
    require(worker_count(0, 1, 9) == 1 && worker_count(0, 2, 9) == 1 &&
            worker_count(0, 7, 9) == 3 && worker_count(0, 16, 3) == 3 &&
            worker_count(0, 0, 9) == 2 && worker_count(4, 1, 9) == 4 &&
            worker_count(64, 1, 1) == 1 && worker_count(0, 0, 0) == 0,
            "Worker policy does not match the contract");
    throws<AudioError>([] { (void)worker_count(65, 32, 100); }, "Excessive override accepted");
}

void removal() {
    TempDirectory tmp;
    BatchEngine engine(backend(), tmp.path);
    const auto first = engine.add(tmp.path / "a.flac").id;
    const auto second = engine.add(tmp.path / "b.flac").id;
    const auto third = engine.add(tmp.path / "c.flac").id;
    require(engine.remove(second) && !engine.remove(second), "Individual removal failed");
    require(engine.add(tmp.path / "c.flac").id == third, "Removal damaged duplicate indices");
    const auto readded = engine.add(tmp.path / "b.flac").id;
    require(readded > third && engine.snapshot().jobs.front().job.id() == first,
            "Removal reused or changed stable IDs");
    engine.start_async(); engine.wait();
    require(engine.remove(first) && fs::exists(tmp.path / "a.mp3"), "Removing a result deleted its output");
    engine.clear();
    require(engine.snapshot().jobs.empty() && engine.add(tmp.path / "a.flac").id > readded,
            "Clearing reused identifiers");
}

void inspection() {
    TempDirectory tmp;
    std::atomic<bool> entered = false;
    auto work = backend();
    work.inspect = [&](const SourceFile& source, const AudioSettings& settings, std::stop_token stop) {
        entered = true;
        while (!stop.stop_requested()) std::this_thread::sleep_for(2ms);
        auto result = backend().inspect(source, settings, stop);
        result.state = JobState::Cancelled; result.diagnostic = {"cancelled", {}, {}};
        return result;
    };
    BatchEngine engine(work, tmp.path);
    const auto id = engine.add(tmp.path / "a.wav").id;
    engine.inspect_async();
    await([&] { return entered.load(); });
    require(engine.snapshot().phase == BatchPhase::Inspecting && !engine.snapshot().jobs[0].progress_visible,
            "Inspection is blocking or shows conversion progress");
    throws<AudioError>([&] { (void)engine.remove(id); }, "Removal allowed during work");
    throws<AudioError>([&] { engine.configure({}); }, "Settings changed during work");
    throws<AudioError>([&] { engine.clear(); }, "Clear allowed during work");
    throws<AudioError>([&] { (void)engine.add(tmp.path / "b.wav"); }, "Add allowed during work");
    throws<AudioError>([&] { engine.start_async(); }, "Concurrent run allowed");
    engine.cancel(); engine.wait();
    require(engine.snapshot().counts.cancelled == 1 && fs::is_empty(tmp.path),
            "Cancelled inspection left work or files behind");
}

void concurrency() {
    TempDirectory tmp;
    std::atomic<int> active = 0, maximum = 0;
    std::atomic<bool> release = false;
    auto work = backend();
    work.convert = [&](const Inspection&, const AudioSettings&, OutputTransaction& output,
                       std::stop_token stop, ProgressCallback) {
        const auto current = ++active;
        auto observed = maximum.load();
        while (observed < current && !maximum.compare_exchange_weak(observed, current)) {}
        while (!release && !stop.stop_requested()) std::this_thread::sleep_for(2ms);
        --active;
        return success(output);
    };
    BatchEngine engine(work, tmp.path);
    AppSettings settings; settings.workers = 2;
    engine.configure(settings);
    fs::create_directories(tmp.path / "left"); fs::create_directories(tmp.path / "right");
    for (const auto& name : {"left/song.flac", "right/song.flac", "last.wav"}) (void)engine.add(tmp.path / name);
    settings.output_mode = OutputMode::CustomDirectory; settings.custom_output_directory = tmp.path;
    engine.configure(settings);
    engine.start_async();
    await([&] { return active == 2; });
    const auto snapshot = engine.snapshot();
    release = true; engine.wait();
    require(snapshot.workers == 2 && snapshot.counts.converting == 2 && maximum == 2 &&
            engine.snapshot().counts.completed == 3, "Worker bound or batch completion incorrect");
    std::set<fs::path> outputs;
    for (const auto& item : engine.snapshot().jobs) outputs.insert(*item.job.output());
    require(outputs.size() == 3 && fs::exists(tmp.path / "song.mp3") && fs::exists(tmp.path / "song (1).mp3"),
            "Central destination allocation collided");
}

void errors() {
    TempDirectory tmp;
    auto work = backend();
    work.inspect = [](const SourceFile& source, const AudioSettings& settings, std::stop_token stop) {
        auto result = backend().inspect(source, settings, stop);
        if (source.path.stem() == "probe_failure") throw std::runtime_error("probe failed");
        if (source.path.stem() == "unsupported") {
            result.state = JobState::UnsupportedSkipped; result.diagnostic = {"unsupported", {}, "bad source"};
        }
        return result;
    };
    work.convert = [](const Inspection& input, const AudioSettings&, OutputTransaction& output,
                      std::stop_token, ProgressCallback) {
        test::write(output.temporary_path(), "partial");
        if (input.source.path.stem() == "failure") throw std::runtime_error("encoder failed");
        return success(output);
    };
    BatchEngine engine(work, tmp.path);
    for (const auto& name : {"failure.wav", "unsupported.wav", "probe_failure.wav", "success.wav", "missing/place.wav"}) {
        (void)engine.add(tmp.path / name);
    }
    engine.start_async(); engine.wait();
    const auto result = engine.snapshot();
    require(result.counts.completed == 1 && result.counts.failed == 2 && result.counts.skipped == 2 &&
            result.warnings.empty(), "Per-file errors aborted or contaminated other jobs");
    no_partials(tmp.path);
}

void progress() {
    TempDirectory tmp;
    std::atomic<bool> release = false, entered = false;
    auto work = backend();
    work.convert = [&](const Inspection& input, const AudioSettings&, OutputTransaction& output,
                       std::stop_token stop, ProgressCallback update) {
        update({input.source.path.stem() == "unknown" ? std::nullopt : std::optional<double>(0.25), 100ms});
        entered = true;
        while (!release && !stop.stop_requested()) std::this_thread::sleep_for(2ms);
        return success(output);
    };
    BatchEngine engine(work, tmp.path);
    (void)engine.add(tmp.path / "known.wav"); (void)engine.add(tmp.path / "unknown.wav");
    AppSettings settings; settings.workers = 2; engine.configure(settings);
    engine.inspect_async(); engine.wait();
    for (const auto& item : engine.snapshot().jobs) require(!item.progress_visible, "ETA visible before start");
    engine.start_async();
    await([&] { return entered && engine.snapshot().counts.converting == 2; });
    await([&] { return engine.snapshot().jobs[0].eta_seconds.has_value(); });
    const auto active = engine.snapshot();
    release = true; engine.wait();
    require(active.jobs[0].job.progress() == 0.25 && active.jobs[0].eta_seconds > 0 &&
            !active.jobs[1].job.progress() && !active.jobs[1].eta_seconds,
            "Percentage/ETA did not distinguish known and unknown progress");
    for (const auto& item : engine.snapshot().jobs) require(item.job.progress() == 1 && item.eta_seconds == 0,
                                                          "Published output did not reach 100%");
}

void cancellation() {
    TempDirectory tmp;
    std::atomic<bool> entered = false;
    auto work = backend();
    work.convert = [&](const Inspection& input, const AudioSettings&, OutputTransaction& output,
                       std::stop_token stop, ProgressCallback) {
        if (input.source.path.stem() == "fast") return success(output);
        test::write(output.temporary_path(), "partial"); entered = true;
        while (!stop.stop_requested()) std::this_thread::sleep_for(2ms);
        return ConversionResult{JobState::Cancelled, {"cancelled", {}, {}}, {}, {}};
    };
    BatchEngine engine(work, tmp.path);
    AppSettings settings; settings.workers = 1; engine.configure(settings);
    for (const auto& name : {"fast.wav", "running.wav", "waiting.wav"}) (void)engine.add(tmp.path / name);
    engine.start_async(); await([&] { return entered.load(); });
    const auto before = std::chrono::steady_clock::now(); engine.cancel();
    require(std::chrono::steady_clock::now() - before < 200ms, "Cancellation blocked the caller");
    engine.wait();
    require(engine.snapshot().counts.completed == 1 && engine.snapshot().counts.cancelled == 2 &&
            test::read(tmp.path / "fast.mp3") == "encoded audio", "Cancellation lost completed output");
    no_partials(tmp.path);
}

void child_cancellation() {
    TempDirectory tmp;
    std::atomic<int> launched = 0;
    auto work = backend();
    work.convert = [&](const Inspection&, const AudioSettings&, OutputTransaction& output,
                       std::stop_token stop, ProgressCallback) {
        test::write(output.temporary_path(), "partial");
        ProcessRequest request{helper, {"sleep"}};
        request.on_stdout = [&](std::string_view) { ++launched; };
        const auto result = run_process(request, stop);
        require(result.cancelled, "Child was not cancelled");
        return ConversionResult{JobState::Cancelled, {"cancelled", {}, {}}, {}, {}};
    };
    BatchEngine engine(work, tmp.path);
    AppSettings settings; settings.workers = 2; engine.configure(settings);
    (void)engine.add(tmp.path / "a.wav"); (void)engine.add(tmp.path / "b.wav");
    engine.start_async(); await([&] { return launched == 2; });
    const auto before = std::chrono::steady_clock::now(); engine.cancel(); engine.wait();
    require(std::chrono::steady_clock::now() - before < 3s && engine.snapshot().counts.cancelled == 2,
            "Running children did not terminate promptly");
    require(fs::is_empty(tmp.path), "Child cancellation left temporary files");
}

void reconfigure() {
    TempDirectory tmp;
    BatchEngine engine(backend(), tmp.path);
    (void)engine.add(tmp.path / "song.wav"); engine.inspect_async(); engine.wait();
    require(engine.snapshot().counts.ready == 1, "Inspection did not classify ready files");
    auto settings = engine.settings(); settings.audio.force_upsampling = true; engine.configure(settings);
    require(engine.snapshot().counts.checking == 1 && !engine.snapshot().jobs[0].media,
            "Settings change did not invalidate inspection");
    engine.start_async(); engine.wait();
    require(engine.snapshot().counts.completed == 1, "Invalidated job was not reinspected");
}

void committed_cancellation() {
    TempDirectory tmp;
    std::atomic<bool> published = false;
    auto work = backend();
    work.convert = [&](const Inspection&, const AudioSettings&, OutputTransaction& output,
                       std::stop_token stop, ProgressCallback) {
        (void)success(output); published = true;
        while (!stop.stop_requested()) std::this_thread::sleep_for(2ms);
        return ConversionResult{JobState::Cancelled, {"cancelled", {}, {}}, {}, {}};
    };
    BatchEngine engine(work, tmp.path);
    (void)engine.add(tmp.path / "song.wav"); engine.start_async();
    await([&] { return published.load(); }); engine.cancel(); engine.wait();
    require(engine.snapshot().counts.completed == 1 && engine.snapshot().counts.cancelled == 0 &&
            test::read(*engine.snapshot().jobs[0].job.output()) == "encoded audio",
            "Cancellation changed a published result into an unfinished one");
}

void destruction() {
    TempDirectory tmp;
    std::atomic<bool> entered = false;
    auto work = backend();
    work.convert = [&](const Inspection&, const AudioSettings&, OutputTransaction& output,
                       std::stop_token stop, ProgressCallback) {
        test::write(output.temporary_path(), "partial"); entered = true;
        while (!stop.stop_requested()) std::this_thread::sleep_for(2ms);
        return ConversionResult{JobState::Cancelled, {"cancelled", {}, {}}, {}, {}};
    };
    {
        BatchEngine engine(work, tmp.path);
        (void)engine.add(tmp.path / "song.wav"); engine.start_async();
        await([&] { return entered.load(); });
    }
    require(fs::is_empty(tmp.path), "Controller destruction did not stop and clean up owned work");
}

void folder_mp3() {
    TempDirectory tmp;
    auto work = backend();
    work.inspect = [](const SourceFile& source, const AudioSettings& settings, std::stop_token stop) {
        auto result = backend().inspect(source, settings, stop);
        if (source.origin == ImportOrigin::FolderScan && !settings.include_mp3_from_folders) {
            result.state = JobState::UnsupportedSkipped; result.diagnostic = {"mp3_excluded", {}, {}};
        }
        return result;
    };
    BatchEngine engine(work, tmp.path);
    (void)engine.add(tmp.path / "selected.mp3", ImportOrigin::SelectedFile);
    const auto folder = engine.add(tmp.path / "folder.mp3", ImportOrigin::FolderScan).id;
    engine.inspect_async(); engine.wait();
    require(engine.snapshot().jobs.size() == 1 && engine.snapshot().counts.excluded == 1,
            "Excluded folder MP3 joined the accepted queue");
    const auto repeated = engine.add(tmp.path / "folder.mp3", ImportOrigin::FolderScan);
    require(!repeated.added && repeated.id == folder, "Excluded sources lost their queue identity");
    engine.inspect_pending_async(); engine.wait();
    require(engine.snapshot().counts.excluded == 1, "Reimport inflated the exclusion count");
    auto settings = engine.settings();
    settings.audio.include_mp3_from_folders = true;
    engine.configure(settings);
    engine.inspect_pending_async(); engine.wait();
    auto restored = engine.snapshot();
    require(restored.jobs.size() == 2 && restored.jobs.back().job.id() == folder && restored.counts.excluded == 0,
            "Enabling folder MP3s did not restore the existing source");
    settings.audio.include_mp3_from_folders = false;
    engine.configure(settings);
    engine.inspect_pending_async(); engine.wait();
    require(engine.snapshot().jobs.size() == 1 && engine.snapshot().counts.excluded == 1,
            "Disabling folder MP3s did not restore the unique exclusion count");
    require(engine.add(tmp.path / "folder.mp3").origin_changed, "Hidden MP3 could not be selected explicitly");
    engine.inspect_pending_async(); engine.wait();
    require(engine.snapshot().counts.ready == 2 && engine.snapshot().counts.excluded == 0,
            "Explicit selection did not remove the previous exclusion");
    const auto id = engine.add(tmp.path / "promoted.mp3", ImportOrigin::FolderScan).id;
    require(engine.add(tmp.path / "promoted.mp3").origin_changed, "Explicit import did not promote origin");
    engine.inspect_async(); engine.wait();
    require(engine.snapshot().jobs.back().job.id() == id && engine.snapshot().counts.ready == 3,
            "Explicit import was subjected to folder exclusion");
}

void logging() {
    TempDirectory tmp;
    auto work = backend();
    work.convert = [](const Inspection&, const AudioSettings&, OutputTransaction& output,
                      std::stop_token, ProgressCallback) {
        auto result = success(output);
        result.warnings.push_back({"metadata_unmapped", {{"field", "PRIVATE"}}, "line\n\x1b raw"});
        return result;
    };
    BatchEngine engine(work, tmp.path, "0.1.0-test");
    (void)engine.add(tmp.path / fs::path(u8"音楽.wav"));
    engine.start_async(); engine.wait(); require(!engine.snapshot().log_path, "Logging was not off by default");
    auto settings = engine.settings(); settings.logging_enabled = true; engine.configure(settings);
    engine.start_async(); engine.wait();
    const auto result = engine.snapshot();
    require(result.log_path && result.warnings.empty(), "Log was not created");
    const auto text = test::read(*result.log_path);
    for (const auto& required : {"Mak3P conversion run:", "UTC", "0.1.0-test", "ffmpeg version fixture", "bitrate_kbps=320",
                                 "音楽.wav", "PRIVATE", "\\x0a", "Converted: 1"}) {
        require(text.find(required) != std::string::npos, std::string("Log missing: ") + required);
    }
    require(result.log_path->parent_path() == tmp.path, "Log used an undisclosed directory");
    require(result.log_path->filename().string().starts_with("Mak3P_"), "Log filename did not use Mak3P");
}

void log_collision() {
    TempDirectory tmp;
    RunLog first(tmp.path, "1", "ffmpeg", "ffprobe", {}, {});
    first.finish(1, 0, 0, 0, 0);
    const auto original = test::read(first.path());
    RunLog second(tmp.path, "2", "ffmpeg", "ffprobe", {}, {});
    require(first.path() != second.path() && test::read(first.path()) == original,
            "A log collision overwrote the earlier report");
}

void log_failure() {
    TempDirectory tmp;
    BatchEngine engine(backend(), tmp.path / "missing");
    AppSettings settings; settings.logging_enabled = true; engine.configure(settings);
    (void)engine.add(tmp.path / "song.wav"); engine.start_async(); engine.wait();
    const auto result = engine.snapshot();
    require(result.counts.completed == 1 && !result.log_path && result.warnings.size() == 1 &&
            result.warnings[0].key == "log_create_failed", "Log failure disrupted conversion or was silent");
    for (const auto& entry : fs::directory_iterator(tmp.path)) require(entry.path().extension() != ".log",
                                                                             "Log silently fell back");
}

void log_write_failure() {
#ifndef _WIN32
    TempDirectory tmp;
    auto work = backend();
    work.convert = [](const Inspection&, const AudioSettings&, OutputTransaction& output,
                      std::stop_token, ProgressCallback) {
        auto result = success(output);
        result.warnings.push_back({"metadata_unmapped", {{"field", "x"}}, std::string(5000, 'x')});
        return result;
    };
    struct LimitGuard {
        rlimit previous{};
        using Handler = void (*)(int);
        Handler signal_handler;
        LimitGuard() : signal_handler(std::signal(SIGXFSZ, SIG_IGN)) {
            getrlimit(RLIMIT_FSIZE, &previous);
            auto limit = previous; limit.rlim_cur = 2048;
            if (setrlimit(RLIMIT_FSIZE, &limit) != 0) throw std::runtime_error("Cannot set file limit");
        }
        ~LimitGuard() { setrlimit(RLIMIT_FSIZE, &previous); std::signal(SIGXFSZ, signal_handler); }
    } guard;
    BatchEngine engine(work, tmp.path);
    AppSettings settings; settings.logging_enabled = true; engine.configure(settings);
    (void)engine.add(tmp.path / "song.wav"); engine.start_async(); engine.wait();
    const auto result = engine.snapshot();
    require(result.counts.completed == 1 && result.log_path && result.warnings.size() == 1 &&
            result.warnings[0].key == "log_write_failed", "Mid-run log write failure was silent or fatal");
#endif
}

void empty() {
    TempDirectory tmp;
    BatchEngine engine(backend(), tmp.path);
    engine.start_async(); engine.wait();
    require(engine.snapshot().counts.total == 0 && engine.snapshot().phase == BatchPhase::Idle,
            "Empty queue did not finish");
}

void restart() {
    TempDirectory tmp;
    BatchEngine engine(backend(), tmp.path);
    (void)engine.add(tmp.path / "song.wav");
    engine.start_async(); engine.wait();
    const auto first = *engine.snapshot().jobs[0].job.output();
    engine.start_async(); engine.wait();
    require(engine.snapshot().counts.completed == 1 && *engine.snapshot().jobs[0].job.output() != first &&
            fs::exists(first), "A repeated batch lost or overwrote its previous output");
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    helper = argv[2];
    return test::run(argc, argv, {{"worker_policy", worker_policy}, {"removal", removal},
        {"inspection", inspection}, {"concurrency", concurrency}, {"errors", errors},
        {"progress", progress}, {"cancellation", cancellation}, {"child_cancellation", child_cancellation},
        {"committed_cancellation", committed_cancellation}, {"destruction", destruction},
        {"reconfigure", reconfigure}, {"folder_mp3", folder_mp3}, {"logging", logging},
        {"log_collision", log_collision}, {"log_failure", log_failure}, {"log_write_failure", log_write_failure},
        {"empty", empty}, {"restart", restart}});
}
