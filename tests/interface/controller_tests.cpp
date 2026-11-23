#include "app/gui_controller.hpp"
#include "app/import.hpp"
#include "gui/presentation.hpp"
#include "audio/process.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <atomic>

using namespace audio_converter;
using namespace std::chrono_literals;
using test::require;
using test::TempDirectory;
namespace {
fs::path fixtures;
fs::path fixture_ffmpeg;
EnginePaths engines;

BatchBackend engine_backend() {
    return batch_backend(std::make_shared<AudioProcessor>(engines));
}

template<class Predicate> GuiSnapshot await(GuiController& controller, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    for (;;) {
        auto snapshot = controller.snapshot();
        require(snapshot.phase != GuiPhase::Idle || snapshot.batch.phase == BatchPhase::Idle,
                "Idle controller published an in-progress batch snapshot");
        if (predicate(snapshot)) return snapshot;
        if (std::chrono::steady_clock::now() > deadline) {
            std::string notices; for (const auto& item : snapshot.notices) notices += message(item) + item.technical_detail;
            throw std::runtime_error("Controller timed out: phase " + std::to_string(static_cast<int>(snapshot.phase)) + " " + notices);
        }
        std::this_thread::sleep_for(3ms);
    }
}
GuiSnapshot idle(GuiController& controller, std::size_t count) {
    return await(controller, [=](const auto& value) { return value.phase == GuiPhase::Idle && value.batch.jobs.size() == count; });
}
BatchBackend backend(std::atomic<int>* conversions = nullptr, bool slow = false) {
    return {
        [](const SourceFile& source, const AudioSettings& settings, std::stop_token) {
            Inspection input; input.source = source; input.settings = settings;
            input.state = JobState::Ready; input.diagnostic = {"ready", {}, {}};
            input.output_bitrate_kbps = settings.bitrate_kbps; input.source_size = 120000;
            MediaInfo media; media.audio.codec = "flac"; media.audio.sample_rate = 44100;
            media.audio.channels = 1; media.duration_seconds = 12; media.bitrate_bps = 88000; input.media = media;
            if (source.path.stem() == "bad") { input.state = JobState::UnsupportedSkipped; input.diagnostic = {"no_audio", {}, "fixture detail"}; }
            return input;
        },
        [=](const Inspection& input, const AudioSettings&, OutputTransaction& output, std::stop_token stop, ProgressCallback progress) {
            if (conversions) ++*conversions;
            for (int i = 0; i < (slow ? 200 : 2); ++i) {
                if (stop.stop_requested()) return ConversionResult{JobState::Cancelled, {"cancelled", {}, {}}, {}, {}};
                if (progress) progress({static_cast<double>(i) / (slow ? 200 : 2), 0us});
                std::this_thread::sleep_for(2ms);
            }
            if (input.source.path.stem() == "fail") return ConversionResult{JobState::Failed, {"encode_failed", {}, "fixture encode failure"}, {}, {}};
            test::write(output.temporary_path(), "encoded fixture");
            return ConversionResult{JobState::Completed, {"completed", {}, {}}, output.commit(), {}};
        }, "ffmpeg version fixture", "ffprobe version fixture"
    };
}
void settings() {
    TempDirectory tmp;
    auto result = parse_settings("", tmp.path);
    require(result.settings.audio.bitrate_kbps == 320 && result.settings.audio.channel_mode == ChannelMode::Stereo,
        "Fresh settings are not 320 kbps stereo");
    result = parse_settings("[conversion]\nbitrate_kbps=192\nchannel_mode=auto\n[window]\nwidth=820\nheight=750\n[engine]\ndirectory=\"tools with spaces\"\n", tmp.path);
    require(result.warnings.empty() && result.settings.audio.bitrate_kbps == 192 && result.settings.audio.channel_mode == ChannelMode::Auto &&
        result.settings.window.width == 820 && result.settings.engine_directory == tmp.path / "tools with spaces", "Saved choices were lost");
    result.settings.logging_enabled = true;
    require(parse_settings(serialize_settings(result.settings), tmp.path).settings == result.settings, "Shared save lost window/engine settings");
    const auto bad = parse_settings("[window]\nwidth=499\nheight=999999\n[logging]\nenabled=true\n", tmp.path);
    require(bad.warnings.size() == 2 && bad.settings.window == WindowPreferences{} && bad.settings.logging_enabled, "Invalid window values contaminated independent settings");
}
void imports() {
    TempDirectory tmp; fs::create_directory(tmp.path / "nested");
    test::write(tmp.path / "a.unknown", "audio"); test::write(tmp.path / "nested" / "b.MP3", "audio"); test::write(tmp.path / "correction.wvc", "sidecar");
    const auto all = discover_sources({tmp.path}, true);
    require(all.files.size() == 2 && all.files[0].path.filename() == "a.unknown" && all.files[1].origin == ImportOrigin::FolderScan, "Folder traversal lost order/content candidates");
    require(discover_sources({tmp.path}, false).files.size() == 1, "Recursion control failed");
    require(discover_sources({tmp.path / "correction.wvc"}, true).files.front().origin == ImportOrigin::SelectedFile, "Explicit files were silently filtered");
    const auto paths = dropped_paths("file:///tmp/Music%20%C3%A9.flac\r\n/tmp/literal%20.wav\n");
    require(paths.size() == 2 && path_utf8(paths[0]) == "/tmp/Music é.flac" && path_utf8(paths[1]) == "/tmp/literal%20.wav", "URI or literal path decoding failed");
    for (const auto* value : {"https://example.com/a.wav", "file://server/a.wav", "file:///tmp/%zz.wav", "file:///tmp/%00.wav", ""})
        test::throws<AudioError>([&] { (void)dropped_paths(value); }, "Invalid/remote drop accepted");
    std::stop_source stop; stop.request_stop(); require(discover_sources({tmp.path}, true, stop.get_token()).files.empty(), "Import ignored cancellation");
#ifndef _WIN32
    fs::create_directory_symlink(tmp.path, tmp.path / "nested" / "loop");
    require(discover_sources({tmp.path}, true).files.size() == 2, "Directory symlink loop traversed");
#endif
}
void engine_discovery() {
    TempDirectory tmp; fs::create_directory(tmp.path / "bin"); fs::create_directory(tmp.path / "chosen");
#ifdef _WIN32
    const auto ffmpeg = "ffmpeg.exe", ffprobe = "ffprobe.exe";
#else
    const auto ffmpeg = "ffmpeg", ffprobe = "ffprobe";
#endif
    for (const auto& directory : {tmp.path / "chosen", tmp.path / "bin", tmp.path}) {
        test::write(directory / ffmpeg, "fixture"); test::write(directory / ffprobe, "fixture");
    }
    require(discover_engine(tmp.path, tmp.path / "chosen").ffmpeg == tmp.path / ffmpeg, "Adjacent engine precedence changed");
    fs::remove(tmp.path / ffprobe);
    require(discover_engine(tmp.path, tmp.path / "chosen").ffmpeg == tmp.path / "bin" / ffmpeg, "Engine pair was mixed across locations");
}
void engine_recovery() {
    TempDirectory tmp;
#ifdef _WIN32
    const auto ffmpeg = "ffmpeg.exe", ffprobe = "ffprobe.exe";
#else
    const auto ffmpeg = "ffmpeg", ffprobe = "ffprobe";
#endif
    test::write(tmp.path / ffmpeg, "broken engine"); test::write(tmp.path / ffprobe, "broken engine");
    GuiController controller(tmp.path);
    auto result = await(controller, [](const auto& value) { return value.phase == GuiPhase::EngineMissing; });
    require(!result.can_start && !result.notices.empty(), "Missing engine did not disable conversion or explain failure");
    const auto selected = engines.ffmpeg.parent_path();
    controller.select_engine(selected);
    result = await(controller, [](const auto& value) { return value.phase == GuiPhase::Idle; });
    require(result.settings.engine_directory == selected && result.notices.empty(), "Explicit engine did not recover from invalid bundled tools");
    controller.close(); await(controller, [](const auto& value) { return value.phase == GuiPhase::Closed; });
    require(read_settings(tmp.path / "config.ini").settings.engine_directory == selected, "Chosen engine folder was not saved");
}
void bundled_engine_recovery() {
    TempDirectory tmp;
    const auto app = tmp.path / "Mak3P";
    fs::create_directories(app);
    const auto bundled = bundled_engine(app);
    fs::copy_file(engines.ffmpeg, app / bundled.ffmpeg.filename());
    fs::copy_file(engines.ffprobe, app / bundled.ffprobe.filename());
    AppSettings settings;
    settings.engine_directory = engines.ffmpeg.parent_path();
    test::write(app / "config.ini", serialize_settings(settings));

    GuiController controller(app, std::nullopt, EngineMode::Bundled);
    auto result = await(controller, [](const auto& value) { return value.phase == GuiPhase::EngineMissing; });
    require(!result.can_start && !result.notices.empty() && result.notices.back().key == "engine_bundle_missing",
            "Full edition accepted external tools or failed to explain recovery");

    fs::create_directory(bundled.ffmpeg.parent_path());
    fs::copy_file(engines.ffmpeg, bundled.ffmpeg);
    test::write(bundled.ffprobe, "invalid executable");
    controller.select_engine(settings.engine_directory);
    result = await(controller, [](const auto& value) {
        return value.phase == GuiPhase::EngineMissing && value.notices.size() >= 2;
    });
    require(result.notices.back().key == "engine_bundle_missing", "Corrupt bundled tool lacked recovery instructions");

    fs::copy_file(engines.ffprobe, bundled.ffprobe, fs::copy_options::overwrite_existing);
    controller.select_engine({});
    result = idle(controller, 0);
    require(result.notices.empty(), "Restored bundle did not clear the audio-tool error");
}

void formatting() {
    JobSnapshot row{ConversionJob(8, {fs::path(u8"/tmp/音楽.flac"), ImportOrigin::SelectedFile}), {"no_audio", {}, "Unreadable fixture"}, {}, {}, false, 0, {}};
    row.job.skip_unsupported("No audio");
    auto view = gui::present_row(row, 2, {});
    require(view.name.starts_with("02 · ") && view.status == "Skipped" && view.tooltip.find("No usable audio") != std::string::npos && view.tooltip.find("Unreadable fixture") != std::string::npos, "Skip explanation is not available to the view");
    row.job.reset(); row.job.mark_ready(); row.job.start(); row.job.fail("failure"); row.diagnostic = {"encode_failed", {}, "encoder stopped"};
    view = gui::present_row(row, 1, {});
    require(view.status == "Failed" && view.failed && view.tooltip.find("encoder stopped") != std::string::npos, "Failure was mislabeled or lost its reason");
    require(gui::duration(3723) == "1:02:03" && gui::duration(9) == "00:09", "Duration formatter failed");
    require(gui::worker_options(4, 3) == std::vector<unsigned>({0, 1, 2, 3, 4}), "Worker menu omitted saved count or exceeded CPU count");
    require(gui::worker_options(128, 64).back() == 64, "Manual worker cap lost");
}
void incremental() {
    TempDirectory tmp; std::atomic<int> conversions = 0;
    BatchEngine engine(backend(&conversions), tmp.path);
    auto settings = engine.settings(); settings.logging_enabled = true; engine.configure(settings);
    const auto old = engine.add(tmp.path / "old.wav").id; (void)engine.add(tmp.path / "bad.wav");
    engine.inspect_pending_async(); engine.wait(); engine.start_pending_async(); engine.wait();
    require(conversions == 1 && engine.snapshot().counts.completed == 1, "Initial pending run failed");
    (void)engine.add(tmp.path / "new.wav"); engine.inspect_pending_async(); engine.wait();
    require(engine.snapshot().jobs.front().job.id() == old && engine.snapshot().jobs.front().job.state() == JobState::Completed, "New import reset completed history");
    engine.start_pending_async(); engine.wait();
    require(conversions == 2 && engine.snapshot().counts.completed == 2, "Previously completed file was converted again");
    const auto log = test::read(*engine.snapshot().log_path);
    require(log.find("bad.wav") != log.npos && log.find("No usable audio") != log.npos && log.find("Converted: 1") != log.npos, "Incremental log omitted skipped input or counted old outputs");
    settings.window.width = 800; settings.logging_enabled = false; settings.workers = 1; settings.audio.include_mp3_from_folders = true; engine.configure(settings);
    require(engine.snapshot().counts.completed == 2, "Presentation/worker/log settings invalidated completed audio");
    settings.audio.bitrate_kbps = 192; engine.configure(settings);
    require(engine.snapshot().counts.checking == 3, "Conversion settings did not invalidate the selection");
}
void history() {
    TempDirectory tmp; std::atomic<int> conversions = 0;
    GuiController controller(tmp.path, backend(&conversions)); idle(controller, 0);
    require(controller.add({tmp.path / "one.wav", tmp.path / "bad.wav", tmp.path / "fail.wav"}), "Import rejected");
    auto result = idle(controller, 3); require(result.can_start && result.batch.counts.skipped == 1, "Ready gating failed");
    require(controller.start(), "Start rejected");
    result = await(controller, [](const auto& value) { return value.phase == GuiPhase::Idle && value.has_run && value.batch.counts.completed == 1 && value.batch.counts.failed == 1; });
    require(!result.can_start && !controller.start(), "Finished revision can start repeatedly");
    require(controller.add({tmp.path / "two.wav"}), "Successive addition rejected"); result = idle(controller, 4);
    require(result.batch.counts.completed == 1 && result.batch.counts.ready == 1, "Addition lost completed history");
    require(controller.start(), "Second run rejected");
    result = await(controller, [](const auto& value) { return value.phase == GuiPhase::Idle && value.batch.counts.completed == 2; });
    require(conversions == 3, "A previous completed/failed file ran again");
    require(controller.remove(result.batch.jobs[0].job.id()), "Removal rejected"); idle(controller, 3);
    require(fs::exists(tmp.path / "one.mp3"), "Queue removal deleted completed output");
    controller.window_size(780, 720);
    await(controller, [](const auto& value) { return value.settings.window.width == 780; });
    controller.close(); await(controller, [](const auto& value) { return value.phase == GuiPhase::Closed; });
    require(read_settings(tmp.path / "config.ini").settings.window.height == 720, "Close lost remembered dimensions");
}
void cancellation() {
    TempDirectory tmp; std::atomic<int> conversions = 0;
    GuiController controller(tmp.path, backend(&conversions, true)); idle(controller, 0);
    require(controller.add({tmp.path / "one.wav", tmp.path / "two.wav"}), "Import rejected"); idle(controller, 2);
    require(controller.start(), "Start rejected");
    await(controller, [](const auto& value) { return value.batch.counts.converting > 0; });
    require(!controller.add({tmp.path / "extra.wav"}) && !controller.clear() && !controller.remove(1), "Running queue can be mutated");
    const auto before = std::chrono::steady_clock::now(); controller.cancel();
    auto result = await(controller, [](const auto& value) { return value.phase == GuiPhase::Idle && value.can_start; });
    require(std::chrono::steady_clock::now() - before < 2s && result.batch.jobs.size() == 2 && result.batch.counts.cancelled == 2, "Cancellation blocked or discarded selection/statuses");
    for (const auto& entry : fs::directory_iterator(tmp.path)) require(entry.path().filename().string().find("partial.mp3") == std::string::npos, "Cancellation leaked partial output");
}
void closing() {
    TempDirectory tmp; GuiController controller(tmp.path, backend(nullptr, true)); idle(controller, 0);
    controller.add({tmp.path / "one.wav"}); idle(controller, 1); controller.start();
    await(controller, [](const auto& value) { return value.batch.counts.converting > 0; });
    controller.close(); await(controller, [](const auto& value) { return value.phase == GuiPhase::Closed; });
    require(!fs::exists(tmp.path / "one.mp3"), "Closing published unfinished audio");
}
void real_cancel() {
    TempDirectory tmp;
    const auto source = tmp.path / "long.flac";
    const auto generated = run_process({fixture_ffmpeg, {"-nostdin", "-v", "error", "-f", "lavfi", "-i", "sine=frequency=997:sample_rate=44100", "-t", "960", "-c:a", "flac", path_utf8(source)}, 15s});
    require(generated.exit_code == 0, "Could not prepare real cancellation fixture");
    GuiController controller(tmp.path, engine_backend()); auto result = idle(controller, 0);
    auto settings = result.settings; settings.audio.force_upsampling = true;
    controller.configure(settings); await(controller, [](const auto& value) { return value.phase == GuiPhase::Idle && value.settings.audio.force_upsampling; });
    controller.add({source}); idle(controller, 1); require(controller.start(), "Real cancellation start rejected");
    await(controller, [](const auto& value) { return value.batch.counts.converting && value.batch.jobs.front().job.progress().value_or(0) > 0; });
    controller.cancel(); result = await(controller, [](const auto& value) { return value.phase == GuiPhase::Idle && value.batch.counts.cancelled == 1; });
    require(result.can_start && !fs::exists(tmp.path / "long.mp3"), "Real cancellation published an unfinished output");
    for (const auto& entry : fs::directory_iterator(tmp.path)) require(entry.path().filename().string().find("partial.mp3") == std::string::npos, "Real cancellation leaked temporary output");
}
void real_audio() {
    TempDirectory tmp; fs::create_directory(tmp.path / "outputs");
    for (const auto* file : {"mono.wav", "cbr.mp3", "invalid.mp3"}) fs::copy_file(fixtures / file, tmp.path / file);
    GuiController controller(tmp.path, engine_backend()); auto result = idle(controller, 0);
    auto settings = result.settings; settings.logging_enabled = true; settings.output_mode = OutputMode::CustomDirectory; settings.custom_output_directory = tmp.path / "outputs";
    settings.audio.bitrate_kbps = 256;
    require(controller.configure(settings), "Real settings rejected");
    await(controller, [](const auto& value) { return value.settings.logging_enabled && value.phase == GuiPhase::Idle; });
    require(controller.add({tmp.path / "mono.wav", tmp.path / "cbr.mp3", tmp.path / "invalid.mp3"}), "Real import rejected");
    result = idle(controller, 3); require(result.batch.counts.ready == 1 && result.batch.counts.skipped == 2, "Real content classification failed");
    require(controller.start(), "Real start rejected");
    result = await(controller, [](const auto& value) { return value.phase == GuiPhase::Idle && value.batch.counts.completed == 1; });
    require(analyze_mp3(tmp.path / "outputs" / "mono.mp3").bitrate_kbps == 256 && result.batch.log_path && result.notices.empty(), "Real output/log was invalid");
    require(test::read(*result.batch.log_path).find("invalid.mp3") != std::string::npos, "Real skipped file missing from log");
}

void media_visibility() {
    TempDirectory tmp;
    const auto folder = tmp.path / "media";
    fs::create_directory(folder);
    fs::copy_file(fixtures / "cover.jpg", folder / "AlbumArtSmall.jpg");
    fs::copy_file(fixtures / "cover.jpg", folder / "Folder.jpg");
    test::write(folder / "desktop.ini", "[ViewState]");
    fs::copy_file(fixtures / "cbr.mp3", folder / "audio.jpg");
    fs::copy_file(fixtures / "cover.png", folder / "image.MP3");
    fs::copy_file(fixtures / "mono.wav", tmp.path / "audio.png");
    GuiController controller(tmp.path, engine_backend());
    auto settings = idle(controller, 0).settings;
    settings.audio.include_mp3_from_folders = true;
    settings.audio.bitrate_kbps = 320;
    settings.logging_enabled = true;
    require(controller.configure(settings), "Could not enable folder MP3s");
    await(controller, [](const auto& value) { return value.settings.audio.include_mp3_from_folders; });
    require(controller.add({folder}), "Folder import rejected");
    auto result = await(controller, [](const auto& value) {
        require(value.batch.counts.total == value.batch.jobs.size(), "Visible row count differs");
        for (const auto& row : value.batch.jobs) {
            const auto name = row.job.source().path.filename();
            require(name == "image.MP3" || (name == "audio.jpg" && row.media),
                    "An unconfirmed or nonaudio file appeared in the queue");
        }
        return value.phase == GuiPhase::Idle && value.batch.jobs.size() == 2;
    });
    require(result.batch.counts.ready == 1 && result.batch.counts.skipped == 1,
            "Folder contents were classified incorrectly");
    for (const auto& row : result.batch.jobs) {
        if (row.job.source().path.filename() == "image.MP3") {
            require(row.diagnostic.key == "no_audio", "A PNG with an audio extension lost its skip reason");
        }
    }
    require(controller.add({folder / "Folder.jpg", tmp.path / "audio.png"}), "Explicit file import rejected");
    result = idle(controller, 3);
    require(result.batch.counts.ready == 2 && result.batch.counts.skipped == 1 &&
            result.batch.jobs.back().job.source().path.filename() == "audio.png",
            "Explicit import did not use the same content policy");
    require(controller.start(), "Could not start the filtered batch");
    result = await(controller, [](const auto& value) { return value.phase == GuiPhase::Idle && value.has_run; });
    require(result.last_run.completed == 2 && result.batch.log_path, "Filtered conversion failed");
    const auto log = test::read(*result.batch.log_path);
    require(log.find("image.MP3") != log.npos && log.find("Folder.jpg") == log.npos &&
            log.find("AlbumArtSmall.jpg") == log.npos && log.find("desktop.ini") == log.npos,
            "Filtered files appeared in the conversion log");
}

void folder_mp3_toggle() {
    TempDirectory tmp;
    const auto inputs = tmp.path / "inputs";
    const auto outputs = tmp.path / "outputs";
    fs::create_directories(inputs);
    fs::create_directories(outputs);
    fs::copy_file(fixtures / "cbr.mp3", inputs / "mp3.jpg");
    fs::copy_file(fixtures / "mono.wav", inputs / "mono.wav");
    GuiController controller(tmp.path, engine_backend());
    auto settings = idle(controller, 0).settings;
    settings.output_mode = OutputMode::CustomDirectory;
    settings.custom_output_directory = outputs;
    require(controller.configure(settings), "Destination setup failed");
    await(controller, [&](const auto& value) { return value.settings == settings; });
    require(controller.add({inputs}), "Folder add failed");
    auto result = idle(controller, 1);
    require(result.batch.counts.excluded == 1, "Initial folder exclusion missing");
    require(controller.start(), "Could not convert the visible item");
    result = await(controller, [](const auto& value) {
        return value.phase == GuiPhase::Idle && value.last_run.completed == 1;
    });
    for (const bool include : {true, false, true, false}) {
        settings.audio.include_mp3_from_folders = include;
        require(controller.configure(settings), "Include toggle rejected");
        result = await(controller, [&](const auto& value) {
            return value.phase == GuiPhase::Idle && value.settings == settings &&
                value.batch.jobs.size() == (include ? 2U : 1U);
        });
        require(result.batch.counts.excluded == (include ? 0U : 1U), "Include toggle left a stale exclusion count");
    }
    require(controller.remove(result.batch.jobs.front().job.id()), "Last visible row could not be removed");
    result = idle(controller, 0);
    require(result.batch.counts.excluded == 0 && !result.has_run && result.run_ids.empty() &&
            result.last_run.completed == 0 && fs::exists(outputs / "mono.mp3"),
            "Removing the last visible row did not clear the selection like Clear selection");
    settings.audio.include_mp3_from_folders = true;
    require(controller.configure(settings), "Could not reenable MP3s");
    result = await(controller, [&](const auto& value) { return value.phase == GuiPhase::Idle && value.settings == settings; });
    require(result.batch.jobs.empty(), "A cleared selection restored old hidden entries");
}
}
int main(int argc, char** argv) {
    if (argc != 6) return 2;
    fixtures = fs::absolute(argv[2]);
    engines = {argv[3], argv[4]};
    fixture_ffmpeg = argv[5];
    return test::run(argc, argv, {{"settings", settings}, {"imports", imports}, {"engine_discovery", engine_discovery},
        {"engine_recovery", engine_recovery}, {"bundled_engine_recovery", bundled_engine_recovery}, {"formatting", formatting},
        {"incremental", incremental}, {"history", history}, {"cancellation", cancellation}, {"closing", closing},
        {"real_audio", real_audio}, {"real_cancel", real_cancel}, {"media_visibility", media_visibility},
        {"folder_mp3_toggle", folder_mp3_toggle}});
}
