#include "app/settings.hpp"
#include "app/file_io.hpp"
#include "audio/process.hpp"
#include "test_support.hpp"

#include <limits>

using namespace audio_converter;
using test::require;
using test::throws;
using test::TempDirectory;

namespace {

void defaults() {
    const auto result = parse_settings("; no overrides\n", fs::current_path());
    require(result.warnings.empty() && result.settings == AppSettings{}, "Defaults changed");
    require(!result.settings.audio.force_upsampling && !result.settings.logging_enabled,
            "Protective defaults must apply");
}

void branding() {
    const auto directory = default_settings_directory();
    require(!directory.empty() && directory.filename() == "Mak3P",
            "The default settings directory does not use the official application name");
}

void round_trip() {
    TempDirectory tmp;
    AppSettings settings;
    settings.audio = {320, ChannelMode::Mono, true, true};
    settings.custom_output_directory = tmp.path / fs::path(u8"音楽 ; # \\\" name");
    settings.recursive_folders = false;
    settings.workers = 3;
    settings.logging_enabled = true;
    const auto result = parse_settings(serialize_settings(settings), tmp.path);
    require(result.warnings.empty() && result.settings == settings,
            "INI round trip lost settings or the inactive custom directory");
    settings.output_mode = OutputMode::CustomDirectory;
    require(parse_settings(serialize_settings(settings), tmp.path).settings == settings,
            "Custom output mode did not round trip");
}

void invalid_entries() {
    TempDirectory tmp;
    const auto result = parse_settings("[conversion]\nbitrate_kbps=17\nchannel_mode=three\n"
        "force_upsampling=maybe\noutput_mode=custom_directory\n[processing]\nworkers=-1\n"
        "workers=999999999999999999999\n[logging]\nenabled=true\nunknown=x\nbroken\n", tmp.path);
    require(result.settings.audio.bitrate_kbps == 320 && result.settings.workers == 0 &&
            result.settings.logging_enabled && result.settings.output_mode == OutputMode::SameAsSource &&
            result.warnings.size() >= 8, "Bad values contaminated valid independent settings");
    for (const auto& warning : result.warnings) require(!message(warning).empty(), "Unlocalized warning");
    const auto repeated = parse_settings("[processing]\nworkers=4\nworkers=65\n", tmp.path);
    require(repeated.settings.workers == 4 && repeated.warnings.size() == 2,
            "Invalid duplicate replaced the valid setting");
}

void paths() {
    TempDirectory tmp;
    const auto result = parse_settings("\xef\xbb\xbf[conversion]\r\ncustom_output_directory=music files\r\n"
        "output_mode=custom_directory\n", tmp.path);
    require(result.warnings.empty() && result.settings.custom_output_directory == tmp.path / "music files",
            "Relative paths must resolve against the INI location");
    const auto malformed = parse_settings("[conversion]\ncustom_output_directory=\"unterminated\n", tmp.path);
    require(malformed.settings.custom_output_directory.empty() && !malformed.warnings.empty(),
            "Malformed quoted path accepted");
    require(executable_directory().is_absolute() && fs::is_directory(executable_directory()),
            "Executable location was inferred from working directory");
}

void limits() {
    require(!parse_settings(std::string(1024 * 1024 + 1, 'x'), fs::current_path()).warnings.empty(),
            "Unbounded configuration accepted");
    AppSettings settings;
    settings.workers = maximum_worker_override + 1;
    throws<AudioError>([&] { validate_settings(settings); }, "Unbounded worker override accepted");
    settings.workers = 0;
    settings.output_mode = OutputMode::CustomDirectory;
    throws<AudioError>([&] { validate_settings(settings); }, "Missing custom destination accepted");
}

void persistence() {
    TempDirectory tmp;
    const auto portable = tmp.path / "app";
    fs::create_directory(portable);
    SettingsStore store(portable, tmp.path / "fallback");
    require(store.load().settings == AppSettings{} && !fs::exists(portable / "config.ini"),
            "Reading missing settings created a config file");
    AppSettings settings;
    settings.audio.force_upsampling = true;
    settings.workers = 2;
    require(store.save(settings).empty(), "Cannot save portable settings");
    const auto result = store.load();
    require(result.settings == settings && !result.using_fallback && result.path == portable / "config.ini",
            "Portable settings were not preferred");
    settings.audio.bitrate_kbps = 192;
    require(store.save(settings).empty() && store.load().settings == settings, "Atomic update failed");
    for (const auto& item : fs::directory_iterator(portable)) {
        require(item.path().filename() == "config.ini", "Settings temporary file leaked");
    }
}

void fallback() {
    TempDirectory tmp;
    SettingsStore store(tmp.path / "absent-app-directory", tmp.path / "fallback");
    const auto initial = store.load();
    require(initial.using_fallback && initial.warnings.empty(), "Writable fallback not selected");
    AppSettings settings;
    settings.workers = 4;
    require(store.save(settings).empty() && store.load().settings == settings,
            "Fallback settings did not persist");
}

void fallback_migration() {
#ifndef _WIN32
    TempDirectory tmp;
    const auto portable = tmp.path / "app";
    fs::create_directory(portable);
    test::write(portable / "config.ini", "[processing]\nworkers=3\n");
    fs::permissions(portable, fs::perms::owner_read | fs::perms::owner_exec);
    SettingsStore store(portable, tmp.path / "fallback");
    const auto initial = store.load();
    fs::permissions(portable, fs::perms::owner_all);
    require(initial.using_fallback && initial.settings.workers == 3, "Portable settings were not migrated");
    fs::permissions(portable, fs::perms::owner_read | fs::perms::owner_exec);
    auto settings = initial.settings;
    settings.workers = 5;
    const auto warnings = store.save(settings);
    const auto loaded = store.load();
    fs::permissions(portable, fs::perms::owner_all);
    require(warnings.empty() && loaded.settings.workers == 5 &&
            test::read(portable / "config.ini").find("workers=3") != std::string::npos,
            "Fallback did not override the stale read-only portable config");
#endif
}

void unavailable_io() {
    TempDirectory tmp;
    test::write(tmp.path / "blocker", "keep");
    SettingsStore store(tmp.path / "missing", tmp.path / "blocker" / "config");
    require(!store.load().warnings.empty() && !store.save({}).empty(), "I/O errors silently ignored");
    require(test::read(tmp.path / "blocker") == "keep", "Fallback damaged an existing file");
    require(!read_settings(tmp.path / "missing.ini").warnings.empty(), "Missing explicit INI ignored");
}

void owned_files() {
    TempDirectory tmp;
    const auto path = tmp.path / "owned.tmp";
    {
        detail::ExclusiveFile file(path);
        file.write("original");
        fs::rename(path, tmp.path / "detached");
        test::write(path, "foreign");
    }
    require(test::read(path) == "foreign", "Cleanup removed a foreign replacement");
#ifndef _WIN32
    fs::create_symlink(tmp.path / "must-not-exist", tmp.path / "link");
    throws<fs::filesystem_error>([&] { detail::ExclusiveFile file(tmp.path / "link"); },
                                "Exclusive create followed a dangling symlink");
    require(!fs::exists(tmp.path / "must-not-exist"), "Symlink target was created");
#endif
}

void readonly_config() {
#ifndef _WIN32
    TempDirectory tmp;
    const auto portable = tmp.path / "app";
    fs::create_directory(portable);
    test::write(portable / "config.ini", "[processing]\nworkers=3\n");
    fs::permissions(portable / "config.ini", fs::perms::owner_read);
    SettingsStore store(portable, tmp.path / "fallback");
    const auto initial = store.load();
    auto settings = initial.settings; settings.workers = 5;
    const auto warnings = store.save(settings);
    fs::permissions(portable / "config.ini", fs::perms::owner_all);
    require(initial.using_fallback && initial.settings.workers == 3 && warnings.empty() &&
            test::read(portable / "config.ini").find("workers=3") != std::string::npos,
            "Read-only portable INI was altered or did not use the fallback");
#endif
}

void sanitization() {
    const auto result = detail::safe_log_text(std::string("a\n\x1b\0", 4) + "\xff" + "日本語");
    require(result.find("日本語") != std::string::npos && result.find('\n') == std::string::npos &&
            result.find("\\x1b") != std::string::npos && result.find("\xef\xbf\xbd") != std::string::npos,
            "Log text lost Unicode or retained control/invalid bytes");
    require(detail::safe_log_text(std::string(9000, 'x')).size() < 8300,
            "Diagnostics were not bounded");
}

} // namespace

int main(int argc, char** argv) {
    return test::run(argc, argv, {{"defaults", defaults}, {"branding", branding}, {"round_trip", round_trip},
        {"invalid_entries", invalid_entries}, {"paths", paths}, {"limits", limits},
        {"persistence", persistence}, {"fallback", fallback}, {"fallback_migration", fallback_migration},
        {"unavailable_io", unavailable_io}, {"readonly_config", readonly_config},
        {"owned_files", owned_files}, {"sanitization", sanitization}});
}
