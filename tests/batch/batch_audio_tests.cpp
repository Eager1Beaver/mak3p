#include "app/batch.hpp"
#include "audio/process.hpp"
#include "test_support.hpp"

#include <memory>
#include <array>

using namespace audio_converter;
using test::require;
using test::TempDirectory;

namespace {
fs::path fixtures, ffmpeg, ffprobe, cli, helper;

fs::path low_flac(const fs::path& directory) {
    const auto path = directory / "quiet.flac";
    fs::copy_file(fixtures / path.filename(), path);
    return path;
}

void bitrate_matching() {
    TempDirectory tmp;
    AudioProcessor processor({ffmpeg, ffprobe});
    const auto lossless = processor.inspect({low_flac(tmp.path)});
    require(lossless.state == JobState::Ready && lossless.media->bitrate_bps &&
            *lossless.media->bitrate_bps >= 32000 && *lossless.media->bitrate_bps < 40000 &&
            lossless.output_bitrate_kbps == 32 && lossless.media->bitrate_estimated,
            "Low-bitrate lossless source was not matched to a standard lower rate");
    for (const auto& fixture : {"tone.m4a", "vbr.mp3"}) {
        const auto input = processor.inspect({fixtures / fixture});
        require(input.state == JobState::Ready && input.output_bitrate_kbps &&
                *input.output_bitrate_kbps < 256 &&
                static_cast<std::uint64_t>(*input.output_bitrate_kbps) * 1000 <= *input.media->bitrate_bps,
                std::string("Low-bitrate source was skipped or increased: ") + fixture);
        if (std::string(fixture) == "vbr.mp3") require(input.media->mp3.mode == Mp3BitrateMode::Vbr,
            "Average bitrate was confused with verified matching CBR");
    }
    require(processor.inspect({fixtures / "cbr.mp3"}, {256}).state == JobState::SameBitrateSkipped,
            "The same-bitrate reason was lost");
    require(processor.inspect({fixtures / "mono.wav"}).state == JobState::Ready,
            "Higher-bitrate source was skipped");
}

void bitrate_boundaries() {
    AudioSettings settings{320};
    constexpr std::array rates{32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    for (std::size_t index = 0; index < rates.size(); ++index) {
        const auto bps = static_cast<std::uint64_t>(rates[index]) * 1000;
        require(select_output_bitrate(settings, bps) == rates[index], "Exact CBR boundary was rounded down");
        const auto below = select_output_bitrate(settings, bps - 1);
        require(index ? below == rates[index - 1] : !below, "Bitrate was rounded upward across a boundary");
    }
    settings.bitrate_kbps = 256;
    require(select_output_bitrate(settings, 302000) == 256 &&
            select_output_bitrate(settings, 235000) == 224 &&
            select_output_bitrate(settings, 173000) == 160 &&
            select_output_bitrate(settings, 31999) == std::nullopt &&
            select_output_bitrate(settings, std::nullopt) == 256,
            "Requested examples, minimum, or unknown-bitrate policy failed");
    settings.force_upsampling = true;
    require(select_output_bitrate(settings, 1) == 256 && select_output_bitrate(settings, 173000) == 256,
            "Force did not select the requested target");
}

void effective_bitrate_validation() {
    TempDirectory tmp;
    AudioProcessor converter({ffmpeg, ffprobe});
    AudioSettings settings{256, ChannelMode::Auto};
    for (const auto& [source_bps, expected] : {std::pair{302000, 256}, {235000, 224}, {173000, 160},
                                            {112000, 112}, {32000, 32}, {31999, 0}}) {
        auto probe = tmp.path / ("ffprobe-bitrate-stream-" + std::to_string(source_bps));
#ifdef _WIN32
        probe += ".exe";
#endif
        fs::copy_file(helper, probe);
        AudioProcessor inspector({ffmpeg, probe});
        const auto input = inspector.inspect({fixtures / "mono.wav"}, settings);
        require(input.settings.bitrate_kbps == 256, "Per-file matching mutated the requested settings");
        if (!expected) {
            require(input.state == JobState::LowerBitrateSkipped && !input.output_bitrate_kbps,
                    "Source below 32 kbps was increased without permission");
            continue;
        }
        require(input.state == JobState::Ready && input.output_bitrate_kbps == expected,
                "Inspection did not record the expected effective rate");
        OutputPathPlanner planner;
        OutputTransaction output(planner, {input.source.path, tmp.path});
        const auto result = converter.convert(input, settings, output);
        require(result.state == JobState::Completed && result.output &&
                analyze_mp3(*result.output).bitrate_kbps == expected,
                "Encoding or validation used the requested rate instead of the effective rate");
        auto altered = input;
        altered.output_bitrate_kbps = 320;
        OutputTransaction invalid(planner, {input.source.path, tmp.path});
        const auto rejected = converter.convert(altered, settings, invalid);
        require(rejected.state == JobState::Failed && rejected.diagnostic.key == "settings_changed" &&
                !fs::exists(invalid.temporary_path()), "Changed per-file rate was accepted or leaked a temporary");
    }
}

void matching_mp3() {
    TempDirectory tmp;
    AudioProcessor processor({ffmpeg, ffprobe});
    for (int bitrate : {32, 112, 160, 224}) {
        const auto source = tmp.path / ("low-" + std::to_string(bitrate) + ".mp3");
        require(run_process({ffmpeg, {"-nostdin", "-v", "error", "-i", path_utf8(fixtures / "mono.wav"),
            "-c:a", "libmp3lame", "-b:a", std::to_string(bitrate) + "k", path_utf8(source)}}).exit_code == 0,
            "Cannot create a low-rate MP3 fixture");
        const auto observed = processor.inspect_source({source});
        auto input = classify_source(observed, {256, ChannelMode::Auto});
        require(input.state == JobState::Ready && input.output_bitrate_kbps == bitrate &&
                input.warnings.size() == 1 && input.warnings[0].key == "bitrate_adjusted",
                "Lower-rate CBR should be ready with an adjustment warning");
        if (bitrate == 160) {
            input = classify_source(observed, {224, ChannelMode::Auto});
            require(input.state == JobState::Ready && input.output_bitrate_kbps == 160 &&
                    input.warnings.size() == 1 && input.warnings[0].key == "bitrate_adjusted",
                    "A 160 kbps source incorrectly skipped at a 224 kbps target");
        }
        if (bitrate >= 160) {
            require(classify_source(observed, {bitrate, ChannelMode::Auto}).state == JobState::SameBitrateSkipped,
                    "An exact matching CBR target did not skip");
            input = classify_source(observed, {128, ChannelMode::Auto});
            require(input.state == JobState::Ready && input.output_bitrate_kbps == 128 &&
                    input.warnings.empty(), "A lower target should convert without a rate adjustment");
        }
        AudioSettings settings{256, ChannelMode::Stereo};
        input = processor.inspect({source}, settings);
        require(input.state == JobState::Ready && input.output_bitrate_kbps == bitrate,
                "Effective-rate matching suppressed a requested channel change");
        settings.force_upsampling = true;
        input = processor.inspect({source}, settings);
        require(input.state == JobState::Ready && input.output_bitrate_kbps == 256 && input.warnings.empty(),
                "Force did not restore the target for a lower-rate MP3");
    }
}

void force_upsampling() {
    TempDirectory tmp;
    AudioProcessor processor({ffmpeg, ffprobe});
    const auto source = low_flac(tmp.path);
    AudioSettings settings; settings.force_upsampling = true; settings.bitrate_kbps = 320;
    const auto input = processor.inspect({source}, settings);
    require(input.state == JobState::Ready, "Force upsampling did not permit conversion");
    OutputPathPlanner planner;
    OutputTransaction output(planner, {source, {}});
    const auto result = processor.convert(input, settings, output);
    require(result.state == JobState::Completed && result.output &&
            analyze_mp3(*result.output).bitrate_kbps == 320, "Forced conversion did not publish target CBR");
    settings.bitrate_kbps = 256;
    require(processor.inspect({fixtures / "cbr.mp3"}, settings).state == JobState::SameBitrateSkipped,
            "Force upsampling disabled redundant MP3 detection");
}

void bitrate_sources() {
    TempDirectory tmp;
    for (const auto& kind : {"stream", "container", "multistream", "unknown"}) {
        auto probe = tmp.path / (std::string("ffprobe-bitrate-") + kind);
#ifdef _WIN32
        probe += ".exe";
#endif
        fs::copy_file(helper, probe);
        AudioProcessor processor({ffmpeg, probe});
        const auto input = processor.inspect({fixtures / "mono.wav"});
        if (std::string(kind) == "stream" || std::string(kind) == "container") {
            require(input.state == JobState::Ready && input.media->bitrate_bps == 128000 &&
                    input.output_bitrate_kbps == 128,
                    "Bitrate precedence or audio-only container fallback failed");
            require(input.media->bitrate_estimated == (std::string(kind) == "container"),
                    "Estimated and reported bitrates were not distinguished");
        } else {
            require(input.state == JobState::Ready && !input.media->bitrate_bps && input.warnings.size() == 1 &&
                    input.warnings[0].key == "bitrate_unknown", "Unknown/video bitrate was treated as audio bitrate");
        }
    }
}

void real_batch() {
    TempDirectory tmp;
    const auto input_dir = tmp.path / "inputs", output_dir = tmp.path / "outputs";
    fs::create_directory(input_dir); fs::create_directory(output_dir);
    fs::copy_file(fixtures / "mono.wav", input_dir / "one.wav");
    fs::copy_file(fixtures / "mono.wav", input_dir / "two.wav");
    fs::copy_file(fixtures / "invalid.mp3", input_dir / "bad.mp3");
    fs::copy_file(fixtures / "cbr.mp3", input_dir / "same.mp3");
    fs::copy_file(fixtures / "tone.m4a", input_dir / "low.m4a");
    test::write(output_dir / "one.mp3", "existing");
    auto processor = std::make_shared<AudioProcessor>(EnginePaths{ffmpeg, ffprobe});
    BatchEngine engine(batch_backend(processor), tmp.path);
    AppSettings settings; settings.workers = 2; settings.logging_enabled = true;
    settings.audio.bitrate_kbps = 256;
    settings.output_mode = OutputMode::CustomDirectory; settings.custom_output_directory = output_dir;
    engine.configure(settings);
    for (const auto& name : {"one.wav", "two.wav", "bad.mp3", "same.mp3", "low.m4a"}) (void)engine.add(input_dir / name);
    engine.inspect_async(); engine.wait();
    require(engine.snapshot().counts.ready == 3 && engine.snapshot().counts.skipped == 2,
            "Real batch preinspection misclassified jobs");
    engine.start_async(); engine.wait();
    const auto result = engine.snapshot();
    require(result.counts.completed == 3 && result.counts.skipped == 2 && result.counts.failed == 0 &&
            result.warnings.empty() && result.log_path && test::read(output_dir / "one.mp3") == "existing",
            "Real batch lost error isolation or output protection");
    for (const auto& item : result.jobs) if (item.job.output()) {
        require(analyze_mp3(*item.job.output()).bitrate_kbps == item.output_bitrate_kbps && item.job.progress() == 1,
                "Batch published an invalid target bitrate");
        if (item.job.source().path.stem() == "low") {
            require(item.output_bitrate_kbps && *item.output_bitrate_kbps < 256 &&
                    test::read(*result.log_path).find("Output bitrate: " + std::to_string(*item.output_bitrate_kbps)) !=
                        std::string::npos, "Snapshot/log did not expose the lower effective rate");
        }
    }
}

void cli_batch() {
    TempDirectory tmp;
    fs::copy_file(fixtures / "mono.wav", tmp.path / "one.wav");
    fs::copy_file(fixtures / "mono.wav", tmp.path / "two.wav");
    const auto config = tmp.path / "options.ini";
    test::write(config, "[processing]\nworkers=1\n[conversion]\nbitrate_kbps=192\n");
    const auto result = run_process({cli, {"batch", path_utf8(tmp.path / "one.wav"),
        path_utf8(tmp.path / "two.wav"), "--config", path_utf8(config)}});
    require(result.exit_code == 0 && result.standard_output.find("Converted: 2") != std::string::npos &&
            analyze_mp3(tmp.path / "one.mp3").bitrate_kbps == 192, "Batch CLI did not apply INI settings");
    const auto matched = run_process({cli, {"inspect", path_utf8(fixtures / "tone.m4a"), "--config", path_utf8(config)}});
    require(matched.exit_code == 0 && matched.standard_output.find("Ready.") != std::string::npos &&
            matched.standard_error.find("Output bitrate adjusted") != std::string::npos,
            "CLI did not expose per-file bitrate matching");
    const auto forced = run_process({cli, {"inspect", path_utf8(fixtures / "tone.m4a"), "--config", path_utf8(config), "--force-upsampling"}});
    require(forced.exit_code == 0 && forced.standard_output.find("Ready.") != std::string::npos,
            "CLI did not expose the force override");
    const auto invalid = run_process({cli, {"batch", path_utf8(tmp.path / "one.wav"), "--config", path_utf8(config), "--workers", "65"}});
    require(invalid.exit_code != 0 && invalid.standard_error.find("64") != std::string::npos,
            "CLI accepted an excessive worker override");
    const auto isolated_directory = tmp.path / "application";
    fs::create_directory(isolated_directory);
    const auto isolated_cli = isolated_directory / cli.filename();
    fs::copy_file(cli, isolated_cli);
    const auto single = run_process({isolated_cli, {"convert", path_utf8(tmp.path / "one.wav"),
        "--config", path_utf8(config), "--bitrate", "320", "--log", "--save-settings"}});
    require(single.exit_code == 0 && single.standard_output.find("Converted: 1") != std::string::npos &&
            read_settings(isolated_directory / "config.ini").settings.audio.bitrate_kbps == 320,
            "Single-file conversion did not use the batch engine or persist settings");
    std::size_t logs = 0;
    for (const auto& item : fs::directory_iterator(isolated_directory)) if (item.path().extension() == ".log") ++logs;
    require(logs == 1, "Single-file logging did not use the executable directory");
    const auto version = run_process({cli, {"--version"}});
    const auto help = run_process({cli, {"--help"}});
    require(version.exit_code == 0 &&
            (version.standard_output == "Mak3P 1.0.0\n" || version.standard_output == "Mak3P 1.0.0\r\n") &&
            help.standard_output.find("Mak3P-cli batch") != std::string::npos &&
            cli.filename().string().starts_with("Mak3P-cli"), "CLI branding did not use Mak3P");
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 7) return 2;
    fixtures = argv[2]; ffmpeg = argv[3]; ffprobe = argv[4]; cli = argv[5]; helper = argv[6];
    return test::run(argc, argv, {{"bitrate_matching", bitrate_matching}, {"bitrate_boundaries", bitrate_boundaries},
        {"effective_bitrate_validation", effective_bitrate_validation}, {"matching_mp3", matching_mp3},
        {"force_upsampling", force_upsampling},
        {"bitrate_sources", bitrate_sources}, {"real_batch", real_batch}, {"cli_batch", cli_batch}});
}
