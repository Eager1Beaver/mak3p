#include "app/batch.hpp"
#include "app/identity.hpp"
#include "app/import.hpp"
#include "audio/process.hpp"

#include <charconv>
#include <csignal>
#include <thread>
#include <iostream>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

using namespace audio_converter;

fs::path input_path(const std::string& text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

void print(const std::string& key, std::map<std::string, std::string> arguments = {}) {
    std::cout << message({key, std::move(arguments), {}}) << '\n';
}

void print_detail(const Diagnostic& diagnostic, bool error = false) {
    auto& stream = error ? std::cerr : std::cout;
    stream << message(diagnostic) << '\n';
    if (!diagnostic.technical_detail.empty()) stream << diagnostic.technical_detail << '\n';
}

volatile std::sig_atomic_t interrupted = 0;
void interrupt_handler(int) { interrupted = 1; }


struct InterruptGuard {
    using Handler = void (*)(int);
    Handler previous = std::signal(SIGINT, interrupt_handler);
    InterruptGuard() { interrupted = 0; }
    ~InterruptGuard() { std::signal(SIGINT, previous); }
};

int run(const std::vector<std::string>& arguments) {
    try {
        if (arguments.size() == 2 && arguments[1] == "--help") {
            print("cli_usage");
            return 0;
        }
        if (arguments.size() == 2 && arguments[1] == "--version") {
            print("cli_version", {{"app", application_name}, {"version", application_version}});
            return 0;
        }
        if (arguments.size() < 3 || (arguments[1] != "inspect" &&
            arguments[1] != "convert" && arguments[1] != "batch")) {
            throw AudioError({"cli_invalid_arguments", {}, {}});
        }
        const auto app_directory = executable_directory();
        SettingsStore store(app_directory);
        SettingsResult loaded;

        std::optional<fs::path> config;
        for (std::size_t index = 2; index < arguments.size(); ++index) {
            if (arguments[index] == "--") break;
            if (arguments[index] == "--config") {
                if (++index == arguments.size()) throw AudioError({"cli_invalid_arguments", {}, {}});
                config = input_path(arguments[index]);
            } else if (arguments[index].starts_with("--") &&
                       arguments[index] != "--folder-import" && arguments[index] != "--include-mp3" &&
                       arguments[index] != "--force-upsampling" && arguments[index] != "--log" &&
                       arguments[index] != "--save-settings") ++index;
        }
        if (config) loaded = read_settings(*config);
        else loaded = store.load();
        for (const auto& warning : loaded.warnings) print_detail(warning, true);
        auto settings = loaded.settings;
        EnginePaths tools = discover_engine(app_directory, settings.engine_directory);
        ImportOrigin origin = ImportOrigin::SelectedFile;
        bool save_settings = false;
        bool paths_only = false;

        std::vector<fs::path> inputs;
        for (std::size_t index = 2; index < arguments.size(); ++index) {
            const auto& option = arguments[index];
            if (option == "--" && !paths_only) { paths_only = true; continue; }
            if (paths_only || !option.starts_with("--")) { inputs.push_back(input_path(option)); continue; }
            if (option == "--folder-import") { origin = ImportOrigin::FolderScan; continue; }
            if (option == "--include-mp3") { settings.audio.include_mp3_from_folders = true; continue; }
            if (option == "--force-upsampling") { settings.audio.force_upsampling = true; continue; }
            if (option == "--log") { settings.logging_enabled = true; continue; }
            if (option == "--save-settings") { save_settings = true; continue; }
            if (index + 1 >= arguments.size()) throw AudioError({"cli_invalid_arguments", {}, {}});
            const auto& value = arguments[++index];
            if (option == "--bitrate") {
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(),
                                                         settings.audio.bitrate_kbps);
                if (error != std::errc{} || end != value.data() + value.size()) {
                    throw AudioError({"invalid_bitrate", {}, {}});
                }
            } else if (option == "--channels") {
                if (value == "auto") settings.audio.channel_mode = ChannelMode::Auto;
                else if (value == "stereo") settings.audio.channel_mode = ChannelMode::Stereo;
                else if (value == "mono") settings.audio.channel_mode = ChannelMode::Mono;
                else throw AudioError({"invalid_channels", {}, {}});
            } else if (option == "--output-dir") {
                settings.output_mode = OutputMode::CustomDirectory;
                settings.custom_output_directory = fs::absolute(input_path(value));
            } else if (option == "--workers") {
                settings.workers = 0;
                if (value != "auto") {
                    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), settings.workers);
                    if (error != std::errc{} || end != value.data() + value.size()) {
                        throw AudioError({"invalid_workers", {}, {}});
                    }
                }
            } else if (option == "--ffmpeg") tools.ffmpeg = input_path(value);
            else if (option == "--ffprobe") tools.ffprobe = input_path(value);
            else if (option != "--config") throw AudioError({"cli_invalid_arguments", {}, {}});
        }
        if (inputs.empty() || (arguments[1] != "batch" && inputs.size() != 1)) {
            throw AudioError({"cli_invalid_arguments", {}, {}});
        }
        validate_settings(settings);
        if (save_settings) for (const auto& warning : store.save(settings)) print_detail(warning, true);
        auto processor = std::make_shared<AudioProcessor>(tools);
        if (arguments[1] != "inspect") {
            BatchEngine batch(batch_backend(processor), app_directory);
            batch.configure(settings);
            for (const auto& path : inputs) (void)batch.add(path, origin);
            InterruptGuard interrupt_guard;
            batch.start_async();
            while (batch.snapshot().phase != BatchPhase::Idle) {
                if (interrupted) batch.cancel();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            batch.wait();
            const auto result = batch.snapshot();
            for (const auto& item : result.jobs) {
                print("cli_row", {{"path", path_utf8(item.job.source().path)}, {"status", message(item.diagnostic)}});
                if (item.output_bitrate_kbps) print("cli_output_bitrate", {{"bitrate", std::to_string(*item.output_bitrate_kbps)}});
                if (!item.diagnostic.technical_detail.empty()) std::cerr << item.diagnostic.technical_detail << '\n';
                for (const auto& warning : item.warnings) print_detail(warning, true);
                if (item.job.output()) print("cli_output", {{"path", path_utf8(*item.job.output())}});
            }
            for (const auto& warning : result.warnings) print_detail(warning, true);
            if (result.log_path) print("cli_log", {{"path", path_utf8(*result.log_path)}});
            const auto& count = result.counts;
            print("batch_summary", {{"completed", std::to_string(count.completed)}, {"skipped", std::to_string(count.skipped)},
                {"failed", std::to_string(count.failed)}, {"cancelled", std::to_string(count.cancelled)},
                {"excluded", std::to_string(count.excluded)}});
            return count.cancelled ? 130 : count.failed || !result.warnings.empty() ? 3 : 0;
        }
        const auto input = processor->inspect({inputs.front(), origin}, settings.audio);
        print("cli_input", {{"path", path_utf8(input.source.path)}});
        print("cli_status", {{"status", message(input.diagnostic)}});
        if (input.output_bitrate_kbps) print("cli_output_bitrate", {{"bitrate", std::to_string(*input.output_bitrate_kbps)}});
        if (input.media) {
            const auto& audio = input.media->audio;
            print("cli_audio", {{"codec", audio.codec}, {"rate", std::to_string(audio.sample_rate)},
                                {"channels", std::to_string(audio.channels)}});
            const auto mode = input.media->mp3.mode;
            const auto key = mode == Mp3BitrateMode::Cbr ? "mode_cbr" :
                             mode == Mp3BitrateMode::Vbr ? "mode_vbr" :
                             mode == Mp3BitrateMode::Unknown ? "mode_unknown" : "mode_not_mp3";
            print("cli_bitrate_mode", {{"mode", message({key, {}, {}})}});
        }
        if (input.state != JobState::Ready) {
            if (!input.diagnostic.technical_detail.empty()) {
                std::cerr << input.diagnostic.technical_detail << '\n';
            }
            return input.state == JobState::SameBitrateSkipped || input.state == JobState::LowerBitrateSkipped ? 0 : 2;
        }
        for (const auto& warning : input.warnings) print_detail(warning, true);
        return 0;
    } catch (const AudioError& error) {
        print_detail({"cli_error", {{"message", message(error.diagnostic())}},
                      error.diagnostic().technical_detail}, true);
        return 1;
    } catch (const std::exception& error) {
        print_detail({"cli_error", {{"message", message({"conversion_failed", {}, {}})}},
                      error.what()}, true);
        return 1;
    }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> arguments;
    for (int index = 0; index < argc; ++index) arguments.push_back(audio_converter::path_utf8(argv[index]));
    return run(arguments);
}
#else
int main(int argc, char** argv) {
    return run(std::vector<std::string>(argv, argv + argc));
}
#endif
