#include "app/batch.hpp"
#include "app/settings.hpp"
#include "audio/process.hpp"

#include <nlohmann/json.hpp>
#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#else
#include <fcntl.h>
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#endif

using namespace audio_converter;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

fs::path path(const std::string& text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string state(JobState value) {
    switch (value) {
    case JobState::Checking: return "checking";
    case JobState::Ready: return "ready";
    case JobState::SameBitrateSkipped: return "same_bitrate";
    case JobState::LowerBitrateSkipped: return "below_minimum";
    case JobState::UnsupportedSkipped: return "unsupported";
    case JobState::Converting: return "converting";
    case JobState::Completed: return "completed";
    case JobState::Failed: return "failed";
    case JobState::Cancelled: return "cancelled";
    }
    throw std::runtime_error("Unknown state");
}

Json diagnostic(const Diagnostic& value) {
    return {{"key", value.key}, {"message", message(value)}, {"detail", value.technical_detail}};
}

Json diagnostics(const std::vector<Diagnostic>& values) {
    Json result = Json::array();
    for (const auto& value : values) result.push_back(diagnostic(value));
    return result;
}

Json metadata(const fs::path& output) {
    const auto native = file_io_path(output);
    TagLib::FileRef file(native.c_str(), false);
    Json result{{"text", Json::object()}, {"pictures", Json::array()}};
    if (file.isNull()) return result;
    for (const auto& [key, values] : file.properties()) {
        auto list = Json::array();
        for (const auto& value : values) list.push_back(value.to8Bit(true));
        result["text"][key.to8Bit(true)] = std::move(list);
    }
    for (const auto& picture : file.complexProperties("PICTURE")) {
        const auto bytes = picture.value("data").toByteVector();
        std::string hex;
        constexpr char digits[] = "0123456789abcdef";
        for (unsigned index = 0; index < bytes.size(); ++index) {
            const auto byte = static_cast<unsigned char>(bytes[index]);
            hex += digits[byte >> 4]; hex += digits[byte & 15];
        }
        result["pictures"].push_back({{"mime", picture.value("mimeType").toString().to8Bit(true)},
                                      {"hex", hex}});
    }
    return result;
}

std::size_t encoder_children() {
#ifdef _WIN32
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot inspect Windows subprocesses");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const auto parent = GetCurrentProcessId();
    std::size_t count = 0;
    for (auto found = Process32FirstW(snapshot, &entry); found; found = Process32NextW(snapshot, &entry)) {
        if (entry.th32ParentProcessID == parent &&
            (_wcsicmp(entry.szExeFile, L"ffmpeg.exe") == 0 ||
             _wcsicmp(entry.szExeFile, L"ffmpeg-realtime.exe") == 0)) {
            ++count;
        }
    }
    CloseHandle(snapshot);
    return count;
#else
    std::set<unsigned long> children;
    const auto tasks = fs::path("/proc") / std::to_string(::getpid()) / "task";
    std::error_code error;
    for (const auto& task : fs::directory_iterator(tasks, error)) {
        std::ifstream list(task.path() / "children");
        for (unsigned long pid; list >> pid;) {
            std::ifstream name(fs::path("/proc") / std::to_string(pid) / "comm");
            std::string command;
            std::getline(name, command);
            if (command == "ffmpeg") children.insert(pid);
        }
    }
    return children.size();
#endif
}

#ifdef _WIN32
int realtime_ffmpeg(int argc, wchar_t** argv, fs::path configuration) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    configuration.replace_extension(".path");
    std::ifstream settings(file_io_path(configuration));
    std::string executable;
    if (!std::getline(settings, executable) || executable.empty()) {
        throw std::runtime_error("Cannot read the validation FFmpeg path");
    }
    ProcessRequest request;
    request.executable = path(executable);
    for (int index = 1; index < argc; ++index) request.arguments.push_back(path_utf8(argv[index]));
    auto& args = request.arguments;
    const auto input = std::find(args.begin(), args.end(), "-i");
    if (input != args.end() && std::find(args.begin(), args.end(), "-progress") != args.end() &&
        input + 1 != args.end() && path(*(input + 1)).filename() != "fast.wav") {
        args.insert(input, "-re");
    }
    request.on_stdout = [](std::string_view bytes) {
        std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size())) << std::flush;
    };
    const auto result = run_process(request);
    std::cerr << result.standard_error;
    return result.exit_code;
}
#endif

} // namespace

int run(const fs::path& request_path, const fs::path& result_path) {
    try {
        std::ifstream input(file_io_path(request_path), std::ios::binary);
        const auto config = Json::parse(input);
        const auto app_directory = path(config.at("application_directory"));
        fs::create_directories(app_directory);
        AppSettings settings;
        settings.audio.bitrate_kbps = config.value("bitrate", 192);
        settings.audio.force_upsampling = config.value("force", true);
        settings.audio.include_mp3_from_folders = config.value("include_mp3", false);
        const auto channels = config.value("channels", std::string("auto"));
        settings.audio.channel_mode = channels == "mono" ? ChannelMode::Mono :
                                      channels == "stereo" ? ChannelMode::Stereo : ChannelMode::Auto;
        settings.workers = config.value("workers", 2U);
        settings.logging_enabled = config.value("logging", true);
        if (config.contains("output_directory")) {
            settings.output_mode = OutputMode::CustomDirectory;
            settings.custom_output_directory = path(config.at("output_directory"));
        }
        const auto processor = std::make_shared<AudioProcessor>(EnginePaths{
            path(config.at("ffmpeg")), path(config.at("ffprobe"))});
        auto backend = batch_backend(processor);
        std::atomic<unsigned> active{0}, peak{0};
        const auto convert = backend.convert;
        backend.convert = [&](const Inspection& inspection, const AudioSettings& audio,
                              OutputTransaction& output, std::stop_token stop, ProgressCallback progress) {
            const auto current = active.fetch_add(1) + 1;
            auto observed = peak.load();
            while (observed < current && !peak.compare_exchange_weak(observed, current)) {}
            struct Guard { std::atomic<unsigned>& value; ~Guard() { --value; } } guard{active};
            return convert(inspection, audio, output, stop, std::move(progress));
        };
        BatchEngine engine(std::move(backend), app_directory);
        engine.configure(settings);
        std::vector<JobId> ids;
        unsigned duplicates = 0;
        for (const auto& entry : config.at("inputs")) {
            const auto source = entry.is_string() ? entry.get<std::string>() : entry.at("path").get<std::string>();
            const auto origin = entry.is_object() && entry.value("folder", false) ?
                ImportOrigin::FolderScan : ImportOrigin::SelectedFile;
            const auto result = engine.add(path(source), origin);
            ids.push_back(result.id);
            if (!result.added) ++duplicates;
        }
        unsigned removed = 0;
        for (const auto& index : config.value("remove_indices", Json::array())) {
            if (engine.remove(ids.at(index.get<std::size_t>()))) ++removed;
        }
        for (const auto& source : config.value("readd", Json::array())) {
            (void)engine.add(path(source.get<std::string>()));
        }
        Json persisted;
        if (config.value("persist", false)) {
            SettingsStore store(app_directory, app_directory / "fallback");
            const auto warnings = store.save(settings);
            const auto loaded = store.load();
            persisted = {{"warnings", diagnostics(warnings)},
                {"bitrate", loaded.settings.audio.bitrate_kbps},
                {"force", loaded.settings.audio.force_upsampling},
                {"custom", path_utf8(loaded.settings.custom_output_directory)}};
        }
        if (config.value("inspect_before", false)) { engine.inspect_async(); engine.wait(); }
        const auto started = Clock::now();
        engine.start_async();
        std::size_t peak_children = 0, peak_rows = 0;
        bool cancel_requested = false, progress_monotonic = true, eta_valid = true;
        bool saw_progress = false, saw_eta = false;
        bool late_conflict_created = false;
        double max_snapshot_seconds = 0;
        std::map<JobId, double> previous;
        double cancel_call_seconds = 0;
        for (;;) {
            const auto snapshot_started = Clock::now();
            const auto snapshot = engine.snapshot();
            max_snapshot_seconds = std::max(max_snapshot_seconds,
                std::chrono::duration<double>(Clock::now() - snapshot_started).count());
            peak_children = std::max(peak_children, encoder_children());
            peak_rows = std::max(peak_rows, snapshot.counts.converting);
            for (const auto& item : snapshot.jobs) {
                if (item.job.progress()) {
                    const auto fraction = *item.job.progress();
                    if (previous.contains(item.job.id()) && fraction < previous.at(item.job.id())) progress_monotonic = false;
                    previous[item.job.id()] = fraction;
                    if (fraction > 0 && fraction < 1) saw_progress = true;
                }
                if (item.eta_seconds) {
                    if (!std::isfinite(*item.eta_seconds) || *item.eta_seconds < 0) eta_valid = false;
                    if (*item.eta_seconds > 0) saw_eta = true;
                }
            }
            if (!late_conflict_created && config.contains("late_conflict") && active > 0) {
                const auto conflict = path(config.at("late_conflict"));
                if (fs::exists(conflict)) throw std::runtime_error("The late-conflict fixture already exists");
                std::ofstream outsider(conflict, std::ios::binary);
                outsider << "late external output";
                if (!outsider) throw std::runtime_error("Cannot create late-conflict fixture");
                late_conflict_created = true;
            }
            if (!cancel_requested && config.contains("cancel_after_completed") &&
                snapshot.counts.completed >= config.at("cancel_after_completed").get<std::size_t>() &&
                active >= config.value("cancel_when_active", 1U)) {
                const auto before = Clock::now();
                engine.cancel();
                cancel_call_seconds = std::chrono::duration<double>(Clock::now() - before).count();
                cancel_requested = true;
            }
            if (snapshot.phase == BatchPhase::Idle) break;
            if (Clock::now() - started > std::chrono::seconds(150)) {
                engine.cancel(); engine.wait(); throw std::runtime_error("Batch exceeded 150 seconds");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        engine.wait();
        const auto result = engine.snapshot();
        Json report{{"elapsed_seconds", std::chrono::duration<double>(Clock::now() - started).count()},
            {"peak_workers", peak.load()}, {"peak_ffmpeg_children", peak_children},
            {"peak_converting_rows", peak_rows}, {"cancel_requested", cancel_requested},
            {"cancel_call_seconds", cancel_call_seconds}, {"remaining_ffmpeg_children", encoder_children()},
            {"progress_monotonic", progress_monotonic}, {"eta_valid", eta_valid},
            {"saw_progress", saw_progress}, {"saw_eta", saw_eta}, {"duplicates", duplicates}, {"removed", removed},
            {"max_snapshot_seconds", max_snapshot_seconds}, {"late_conflict_created", late_conflict_created},
            {"warnings", diagnostics(result.warnings)}, {"jobs", Json::array()}, {"persisted", persisted},
            {"ffmpeg_version", processor->ffmpeg_version()}, {"ffprobe_version", processor->ffprobe_version()},
            {"counts", {{"total", result.counts.total}, {"completed", result.counts.completed},
                {"skipped", result.counts.skipped}, {"failed", result.counts.failed},
                {"cancelled", result.counts.cancelled}, {"excluded", result.counts.excluded}}}};
        if (result.log_path) report["log"] = path_utf8(*result.log_path);
        for (const auto& item : result.jobs) {
            Json job{{"id", item.job.id()}, {"source", path_utf8(item.job.source().path)},
                {"state", state(item.job.state())}, {"diagnostic", diagnostic(item.diagnostic)},
                {"warnings", diagnostics(item.warnings)}, {"progress_visible", item.progress_visible},
                {"elapsed_seconds", item.elapsed_seconds}};
            if (item.output_bitrate_kbps) job["effective_bitrate"] = *item.output_bitrate_kbps;
            if (item.job.progress()) job["progress"] = *item.job.progress();
            if (item.job.output()) {
                job["output"] = path_utf8(*item.job.output());
                job["metadata"] = metadata(*item.job.output());
            }
            report["jobs"].push_back(std::move(job));
        }
        std::ofstream output(file_io_path(result_path), std::ios::binary);
        output << report.dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
        if (!output) throw std::runtime_error("Cannot write report");
        std::cout << report.at("counts").dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    const fs::path executable(argv[0]);
    if (executable.filename() == "ffmpeg-realtime.exe") {
        try { return realtime_ffmpeg(argc, argv, executable); }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
    return argc == 3 ? run(argv[1], argv[2]) : 2;
}
#else
int main(int argc, char** argv) {
    return argc == 3 ? run(argv[1], argv[2]) : 2;
}
#endif
