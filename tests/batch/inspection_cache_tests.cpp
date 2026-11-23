#include "app/batch.hpp"
#include "app/gui_controller.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>

using namespace audio_converter;
using namespace std::chrono_literals;
using test::require;
using test::TempDirectory;

namespace {
fs::path fixtures, ffmpeg, ffprobe;

SourceInspection observed(const SourceFile& source, bool mp3 = false) {
    SourceInspection result;
    result.source = source; result.state = JobState::Ready; result.diagnostic = {"ready", {}, {}};
    result.source_size = 123456;
    MediaInfo media; media.format = mp3 ? "mp3" : "flac";
    media.audio.codec = mp3 ? "mp3" : "flac"; media.audio.index = 0;
    media.audio.sample_rate = 44100; media.audio.channels = 2;
    media.duration_seconds = 12; media.bitrate_bps = mp3 ? 128000 : 640000;
    if (mp3) media.mp3 = {Mp3BitrateMode::Cbr, 128, 12};
    result.media = std::move(media); return result;
}

BatchBackend cached_backend(std::atomic<int>& scans) {
    BatchBackend result;
    result.inspect = [](const SourceFile&, const AudioSettings&, std::stop_token) -> Inspection {
        throw std::runtime_error("Combined inspector must not run when source observations are available");
    };
    result.inspect_source = [&](const SourceFile& source, std::stop_token) {
        ++scans; return observed(source, source.path.extension() == ".mp3");
    };
    result.convert = [](const Inspection& input, const AudioSettings& settings, OutputTransaction& output,
                        std::stop_token, ProgressCallback) {
        require(input.settings == settings, "Conversion received a stale policy decision");
        test::write(output.temporary_path(), "encoded");
        return ConversionResult{JobState::Completed, {"completed", {}, {}}, output.commit(), input.warnings};
    };
    return result;
}

void policy() {
    auto raw = observed({"absent.mp3"}, true);
    AudioSettings settings{128};
    require(classify_source(raw, settings).state == JobState::SameBitrateSkipped, "Cached CBR match changed");
    settings.bitrate_kbps = 320; settings.channel_mode = ChannelMode::Mono;
    auto result = classify_source(raw, settings);
    require(result.state == JobState::Ready && result.output_bitrate_kbps == 128 && result.warnings.size() == 1,
            "Cached channel change or bitrate adjustment was not applied");
    settings.force_upsampling = true;
    result = classify_source(raw, settings);
    require(result.output_bitrate_kbps == 320 && result.warnings.empty(), "Force retained an obsolete adjustment");
    raw.source.origin = ImportOrigin::FolderScan;
    require(classify_source(raw, settings).diagnostic.key == "mp3_excluded", "Folder policy ignored cached content");
    settings.include_mp3_from_folders = true;
    require(classify_source(raw, settings).state == JobState::Ready, "Folder policy could not make cached input eligible");

    raw = observed({"absent.wv"}); raw.warnings.push_back({"wavpack_hybrid", {}, {}});
    raw.media->bitrate_bps.reset(); settings = {};
    for (int index = 0; index < 10; ++index) {
        result = classify_source(raw, settings);
        require(result.warnings.size() == 2 && result.warnings[0].key == "wavpack_hybrid" &&
                result.warnings[1].key == "bitrate_unknown", "Cached warnings accumulated or vanished");
    }
    settings.force_upsampling = true;
    require(classify_source(raw, settings).warnings.size() == 1, "Unknown-bitrate warning outlived its policy");
    raw.media->bitrate_bps = 31000; settings.force_upsampling = false;
    require(classify_source(raw, settings).state == JobState::LowerBitrateSkipped, "Cached low bitrate was not skipped");
    settings.force_upsampling = true;
    require(classify_source(raw, settings).state == JobState::Ready, "Force could not enable a cached low-bitrate source");
    require(raw.warnings.size() == 1 && raw.state == JobState::Ready, "Classification mutated source observations");
}

void lifetime() {
    TempDirectory tmp; std::atomic<int> scans = 0;
    BatchEngine engine(cached_backend(scans), tmp.path); AppSettings settings;
    const auto first = engine.add(tmp.path / "one.flac").id;
    engine.inspect_pending_async(); engine.wait();
    require(scans == 1 && engine.snapshot().counts.ready == 1, "Initial source scan did not settle");
    for (const auto rate : {128, 192, 320}) {
        settings.audio.bitrate_kbps = rate; settings.audio.channel_mode = ChannelMode::Mono;
        settings.audio.force_upsampling = !settings.audio.force_upsampling;
        settings.output_mode = OutputMode::CustomDirectory; settings.custom_output_directory = tmp.path;
        engine.configure(settings); engine.inspect_pending_async(); engine.wait();
        const auto row = engine.snapshot().jobs.front();
        require(scans == 1 && row.output_bitrate_kbps == rate && row.source_size == 123456 &&
                row.media->duration_seconds == 12, "Settings change rescanned or lost observed properties");
    }
    require(!engine.add(tmp.path / "one.flac").added, "Duplicate addition created a new row");
    engine.inspect_async(); engine.wait(); engine.start_pending_async(); engine.wait();
    require(scans == 1 && engine.snapshot().counts.completed == 1, "Inspection/conversion start reread the source");
    (void)engine.add(tmp.path / "two.flac"); engine.inspect_pending_async(); engine.wait();
    require(scans == 2 && engine.snapshot().counts.completed == 1 && engine.snapshot().counts.ready == 1,
            "Adding another file rescanned or invalidated completed history");
    require(engine.remove(first), "Removal failed");
    const auto readded = engine.add(tmp.path / "one.flac").id;
    engine.inspect_pending_async(); engine.wait();
    require(readded != first && scans == 3, "Removal retained the old file's cache");
    engine.clear(); (void)engine.add(tmp.path / "one.flac"); engine.inspect_pending_async(); engine.wait();
    require(scans == 4, "Clearing retained source observations");
}

void failures_and_cancellation() {
    TempDirectory tmp; std::atomic<int> scans = 0; auto backend = cached_backend(scans);
    backend.inspect_source = [&](const SourceFile& source, std::stop_token) {
        ++scans; auto raw = observed(source);
        raw.state = JobState::UnsupportedSkipped; raw.media.reset(); raw.diagnostic = {"no_audio", {}, "cached failure"};
        return raw;
    };
    BatchEngine bad(backend, tmp.path); (void)bad.add(tmp.path / "bad.wav");
    bad.inspect_pending_async(); bad.wait(); AppSettings settings; settings.audio.bitrate_kbps = 192;
    bad.configure(settings); bad.inspect_pending_async(); bad.wait();
    require(scans == 1 && bad.snapshot().jobs.front().diagnostic.technical_detail == "cached failure",
            "Settings change rescanned an unsupported source or lost its explanation");
    scans = 0;
    backend.inspect_source = [&](const SourceFile& source, std::stop_token) {
        auto raw = observed(source);
        if (++scans == 1) { raw.state = JobState::Cancelled; raw.diagnostic = {"process_cancelled", {}, {}}; }
        return raw;
    };
    BatchEngine retry(backend, tmp.path); (void)retry.add(tmp.path / "retry.wav");
    retry.inspect_pending_async(); retry.wait();
    require(retry.snapshot().counts.cancelled == 1, "Interrupted initial scan was not cancelled");
    retry.inspect_pending_async(); retry.wait(); retry.start_pending_async(); retry.wait();
    require(scans == 2 && retry.snapshot().counts.completed == 1, "Incomplete scan was cached or complete scan was repeated");
}

void promotion() {
    TempDirectory tmp; std::atomic<int> scans = 0; BatchEngine engine(cached_backend(scans), tmp.path);
    AppSettings settings; settings.audio.bitrate_kbps = 128; settings.audio.include_mp3_from_folders = true; engine.configure(settings);
    const auto id = engine.add(tmp.path / "one.mp3", ImportOrigin::FolderScan).id;
    engine.inspect_pending_async(); engine.wait();
    require(engine.add(tmp.path / "one.mp3").origin_changed, "Explicit addition did not promote folder input");
    settings.audio.include_mp3_from_folders = false; engine.configure(settings);
    engine.inspect_pending_async(); engine.wait();
    require(scans == 1 && engine.snapshot().jobs.front().job.id() == id && engine.snapshot().counts.skipped == 1 &&
            engine.snapshot().jobs.front().diagnostic.key == "same_bitrate", "Promotion lost the cache or retained folder exclusion");
    (void)engine.add(tmp.path / "two.mp3", ImportOrigin::FolderScan); engine.inspect_pending_async(); engine.wait();
    require(scans == 2 && engine.snapshot().counts.excluded == 1 && engine.snapshot().jobs.size() == 1,
            "Excluded folder row remained visible");
    settings.audio.include_mp3_from_folders = true; engine.configure(settings);
    (void)engine.add(tmp.path / "two.mp3", ImportOrigin::FolderScan); engine.inspect_pending_async(); engine.wait();
    require(scans == 2 && engine.snapshot().jobs.size() == 2, "Restoring a hidden source discarded its cache");
    const auto removed = engine.snapshot().jobs.back().job.id();
    require(engine.remove(removed), "Restored row could not be removed");
    require(engine.add(tmp.path / "two.mp3", ImportOrigin::FolderScan).id != removed, "Removed row retained its ID");
    engine.inspect_pending_async(); engine.wait();
    require(scans == 3, "Explicit removal retained the cached source");
}

BatchBackend real_backend(std::atomic<int>& scans) {
    auto processor = std::make_shared<AudioProcessor>(EnginePaths{ffmpeg, ffprobe});
    auto backend = batch_backend(processor); const auto scan = backend.inspect_source;
    backend.inspect_source = [&, scan](const SourceFile& source, std::stop_token stop) { ++scans; return scan(source, stop); };
    return backend;
}

void changed_source() {
    TempDirectory tmp; std::atomic<int> scans = 0;
    const auto source = tmp.path / "one.wav"; fs::copy_file(fixtures / "mono.wav", source);
    BatchEngine engine(real_backend(scans), tmp.path); const auto first = engine.add(source).id;
    engine.inspect_pending_async(); engine.wait(); const auto original_size = engine.snapshot().jobs.front().source_size;
    test::write(source, test::read(source) + "changed");
    AppSettings settings; settings.audio.bitrate_kbps = 224;
    engine.configure(settings); engine.inspect_pending_async(); engine.wait();
    require(scans == 1 && engine.snapshot().jobs.front().source_size == original_size,
            "Settings change reread a changed source instead of using queued observations");
    engine.start_pending_async(); engine.wait();
    require(scans == 1 && engine.snapshot().counts.failed == 1 &&
            engine.snapshot().jobs.front().diagnostic.key == "source_changed" && !fs::exists(tmp.path / "one.mp3"),
            "Conversion trusted changed source data or silently refreshed its cache");
    require(engine.remove(first), "Changed source removal failed");
    fs::copy_file(fixtures / "mono.wav", source, fs::copy_options::overwrite_existing);
    (void)engine.add(source); engine.inspect_pending_async(); engine.wait(); engine.start_pending_async(); engine.wait();
    require(scans == 2 && engine.snapshot().counts.completed == 1 && analyze_mp3(tmp.path / "one.mp3").bitrate_kbps == 224,
            "Readded source did not receive a fresh usable inspection");
    for (const auto& entry : fs::directory_iterator(tmp.path))
        require(entry.path().filename().string().find("partial.mp3") == std::string::npos, "Changed-source failure leaked a temporary");
}

void real_mp3() {
    TempDirectory tmp; std::atomic<int> scans = 0;
    const auto source = tmp.path / "input.mp3"; fs::copy_file(fixtures / "cbr.mp3", source);
    BatchEngine engine(real_backend(scans), tmp.path);
    AppSettings settings; settings.audio.bitrate_kbps = 256; engine.configure(settings);
    (void)engine.add(source);
    engine.inspect_pending_async(); engine.wait();
    require(engine.snapshot().jobs.front().diagnostic.key == "same_bitrate", "Initial matching MP3 was not skipped");
    fs::remove(source); // Cached policy evaluation must work without reopening or statting the source.
    settings.audio.channel_mode = ChannelMode::Mono; engine.configure(settings);
    engine.inspect_pending_async(); engine.wait();
    require(scans == 1 && engine.snapshot().counts.ready == 1, "Cached channel decision accessed the missing file");
    settings.audio.channel_mode = ChannelMode::Stereo; engine.configure(settings); engine.inspect_pending_async(); engine.wait();
    require(scans == 1 && engine.snapshot().jobs.front().diagnostic.key == "same_bitrate", "Cached MP3 match could not be restored");
}

template<class Predicate> GuiSnapshot await(GuiController& controller, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        auto snapshot = controller.snapshot(); if (predicate(snapshot)) return snapshot;
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Controller cache operation timed out");
        std::this_thread::sleep_for(2ms);
    }
}

void controller() {
    TempDirectory tmp; std::atomic<int> scans = 0;
    GuiController controller(tmp.path, cached_backend(scans));
    await(controller, [](const auto& value) { return value.phase == GuiPhase::Idle; });
    auto initial = controller.snapshot().settings; initial.audio.bitrate_kbps = 128;
    require(controller.configure(initial), "GUI initial target rejected");
    await(controller, [&](const auto& value) { return value.phase == GuiPhase::Idle && value.settings == initial; });
    require(controller.add({tmp.path / "one.mp3"}), "GUI addition rejected");
    auto result = await(controller, [](const auto& value) { return value.phase == GuiPhase::Idle && value.batch.counts.skipped == 1; });
    auto settings = result.settings; settings.audio.channel_mode = ChannelMode::Mono;
    require(controller.configure(settings), "GUI settings change rejected");
    result = await(controller, [](const auto& value) { return value.phase == GuiPhase::Idle && value.batch.counts.ready == 1; });
    require(scans == 1 && result.can_start && result.batch.jobs.front().media->audio.channels == 2,
            "GUI reconfiguration rescanned or overwrote observed source channels");
    require(controller.start(), "GUI cached conversion rejected");
    await(controller, [](const auto& value) { return value.phase == GuiPhase::Idle && value.batch.counts.completed == 1; });
    require(scans == 1, "GUI start rescanned the ready source");
    controller.close(); await(controller, [](const auto& value) { return value.phase == GuiPhase::Closed; });
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 5) return 2;
    fixtures = argv[2]; ffmpeg = argv[3]; ffprobe = argv[4];
    return test::run(argc, argv, {{"policy", policy}, {"lifetime", lifetime},
        {"failures_and_cancellation", failures_and_cancellation}, {"promotion", promotion},
        {"changed_source", changed_source}, {"real_mp3", real_mp3}, {"controller", controller}});
}
