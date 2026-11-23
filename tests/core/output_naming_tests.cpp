#include "core/output_naming.hpp"
#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using audio_converter::OutputPathPlanner;
using audio_converter::OutputRequest;

namespace {

void require(bool ok, const std::string& description) {
    if (!ok) {
        throw std::runtime_error(description);
    }
}

struct TempDirectory {
    fs::path path;

    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = fs::temp_directory_path() /
                ("audio-converter-test-" + std::to_string(stamp) + "-" + std::to_string(attempt));
            std::error_code ec;
            if (fs::create_directory(candidate, ec)) {
                path = candidate;
                return;
            }
            if (ec) {
                throw fs::filesystem_error("Cannot create test directory", candidate, ec);
            }
        }
        throw std::runtime_error("Could not allocate unique test directory");
    }

    ~TempDirectory() {
        if (!path.empty()) {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
};

void test_collision_and_reservations() {
    TempDirectory tmp;
    const fs::path output = tmp.path / "output";
    fs::create_directory(output);

    // A real, existing file must never be selected for output.
    std::ofstream(output / "Track.mp3") << "already exists";
    std::ofstream(output / "Track (1).mp3") << "already exists";

    OutputPathPlanner planner;
    const auto first = planner.reserve({tmp.path / "Album A" / "Track.flac", output});
    const auto second = planner.reserve({tmp.path / "Album B" / "Track.ape", output});
    const auto third = planner.reserve({tmp.path / "Album C" / "Track.wv", output});

    require(first.filename() == "Track (2).mp3", "First free suffix should be (2)");
    require(second.filename() == "Track (3).mp3", "Second reservation should be (3)");
    require(third.filename() == "Track (4).mp3", "Third reservation should be (4)");
    require(fs::file_size(output / "Track.mp3") == 14, "Existing file was touched");
}

void test_source_directories_and_mp3_inputs() {
    TempDirectory tmp;
    fs::create_directory(tmp.path / "first");
    fs::create_directory(tmp.path / "second");
    std::ofstream(tmp.path / "first" / "song.mp3") << "original mp3";

    OutputPathPlanner planner;
    const auto mp3 = planner.reserve({tmp.path / "first" / "song.mp3", std::nullopt});
    const auto other = planner.reserve({tmp.path / "second" / "song.flac", std::nullopt});
    require(mp3 == tmp.path / "first" / "song (1).mp3", "MP3 source must not be overwritten");
    require(other == tmp.path / "second" / "song.mp3", "Output should stay beside source");
}

void test_invalid_destination() {
    TempDirectory tmp;
    OutputPathPlanner planner;
    bool threw = false;
    try {
        (void)planner.reserve({tmp.path / "source.flac", tmp.path / "not-created"});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "Missing output directory should be rejected");
    test::write(tmp.path / "file.mp3", "existing");
    test::throws<std::invalid_argument>([&] {
        (void)planner.reserve({tmp.path / "source.flac", tmp.path / "file.mp3"});
    }, "An existing file must not be used as an output directory");
    test::throws<std::invalid_argument>([&] {
        (void)planner.reserve({tmp.path / "source.flac", fs::path()});
    }, "An empty custom destination must not redirect output to the current directory");
}

void test_clear() {
    TempDirectory tmp;
    OutputPathPlanner planner;
    const auto first = planner.reserve({tmp.path / "x.flac", tmp.path});
    planner.clear();
    const auto after_clear = planner.reserve({tmp.path / "x.wav", tmp.path});
    require(first == after_clear, "Clear should reset batch reservations");
}

void test_gaps() {
    TempDirectory tmp;
    test::write(tmp.path / "song.mp3", "existing");
    test::write(tmp.path / "song (2).mp3", "existing");
    OutputPathPlanner planner;
    require(planner.reserve({tmp.path / "song.flac", tmp.path}).filename() == "song (1).mp3",
            "The smallest available suffix must be used");
}

void test_relative() {
    TempDirectory tmp;
    const auto root = fs::canonical(tmp.path);
    fs::create_directory(root / "album");
    test::WorkingDirectory cwd(root);
    OutputPathPlanner planner;
    const auto first = planner.reserve({"album/song.flac", std::nullopt});
    const auto second = planner.reserve({root / "album/./song.wav", root / "album"});
    require(first == root / "album/song.mp3" && second == root / "album/song (1).mp3",
            "Relative and absolute destinations must share reservations");
    require(planner.reserve({"plain.flac", std::nullopt}) == root / "plain.mp3",
            "An input in the current directory must be supported");
}

void test_unicode() {
    TempDirectory tmp;
    OutputPathPlanner planner;
    const fs::path name(u8"Étude 日本語 🎵.flac");
    const auto first = planner.reserve({tmp.path / name, tmp.path});
    const auto second = planner.reserve({tmp.path / name, tmp.path});
    require(first.filename() == fs::path(u8"Étude 日本語 🎵.mp3") &&
            second.filename() == fs::path(u8"Étude 日本語 🎵 (1).mp3"),
            "Unicode basenames must survive suffix generation");
}

void test_long_name() {
    TempDirectory tmp;
    const auto limit = audio_converter::filename_limit(tmp.path);
    const std::string stem(limit - 2, 'a');
    OutputPathPlanner planner;
    const auto first = planner.reserve({tmp.path / (stem + ".a"), tmp.path});
    const auto second = planner.reserve({tmp.path / (stem + ".a"), tmp.path});
    require(first.filename().native().size() <= limit &&
            second.filename().native().size() <= limit && first != second,
            "Long output names must fit with room for collision suffixes");
    test::write(first, "first");
    test::write(second, "second");
    require(test::read(first) == "first" && test::read(second) == "second",
            "Shortened output names must be usable");
    planner.clear();
    const auto third = planner.reserve({tmp.path / (stem + ".a"), tmp.path});
    require(third != first && third != second, "Existing long output names must remain occupied");
    fs::path::string_type unicode_stem;
    const auto unit = fs::path(u8"音").native();
    while (unicode_stem.size() + unit.size() <= limit - 2) {
        unicode_stem += unit;
    }
    const auto unicode = planner.reserve({tmp.path / (fs::path(unicode_stem) += ".a"), tmp.path});
    const auto bytes = unicode.stem().u8string();
    require(bytes.size() % 3 == 0, "Shortening split a Unicode code point");
}

void test_alias() {
#ifndef _WIN32
    TempDirectory tmp;
    fs::create_directory(tmp.path / "real");
    fs::create_directory_symlink(tmp.path / "real", tmp.path / "alias");
    OutputPathPlanner planner;
    const auto first = planner.reserve({tmp.path / "song.flac", tmp.path / "real"});
    const auto second = planner.reserve({tmp.path / "song.wav", tmp.path / "alias"});
    require(first != second && second.filename() == "song (1).mp3",
            "Aliased output directories must share reservations");
#endif
}

void test_occupied_links() {
    TempDirectory tmp;
    fs::create_directory(tmp.path / "song.mp3");
    OutputPathPlanner planner;
#ifndef _WIN32
    fs::create_symlink(tmp.path / "missing-target", tmp.path / "song (1).mp3");
    const auto next = planner.reserve({tmp.path / "song.flac", tmp.path});
    require(next.filename() == "song (2).mp3",
            "Directories and dangling symlinks must occupy output names");
    require(fs::is_symlink(fs::symlink_status(tmp.path / "song (1).mp3")),
            "Planning changed a pre-existing symlink");
#else
    require(planner.reserve({tmp.path / "song.flac", tmp.path}).filename() == "song (1).mp3",
            "An existing directory must occupy the output name");
#endif
}

void test_invalid_sources() {
    TempDirectory tmp;
    OutputPathPlanner planner;
    for (const fs::path& source : {fs::path(), fs::path("."), fs::path(".."), fs::path("dir/")}) {
        test::throws<std::invalid_argument>([&] { (void)planner.reserve({source, tmp.path}); },
                                           "Invalid source filename was accepted");
    }
    const fs::path nul(std::string("song\0.flac", 10));
    test::throws<std::invalid_argument>([&] { (void)planner.reserve({nul, tmp.path}); },
                                       "Embedded NUL was accepted");
}

void test_case_rules() {
    TempDirectory tmp;
    OutputPathPlanner planner;
    const auto first = planner.reserve({tmp.path / fs::path(u8"Äudio.flac"), tmp.path});
    const auto second = planner.reserve({tmp.path / fs::path(u8"äudio.wav"), tmp.path});
#ifdef _WIN32
    require(second.filename() == fs::path(u8"äudio (1).mp3"),
            "Windows Unicode case must share a reservation");
#else
    require(first != second && second.filename() == fs::path(u8"äudio.mp3"),
            "Linux case-sensitive basenames must remain distinct");
#endif
}

void test_windows_sanitization() {
    TempDirectory tmp;
    OutputPathPlanner planner;
#ifdef _WIN32
    require(planner.reserve({tmp.path / "CON.flac", tmp.path}).filename() == "CON_.mp3",
            "Reserved Windows device basename was not sanitized");
    require(planner.reserve({tmp.path / "CON.album.flac", tmp.path}).filename() == "CON_.album.mp3",
            "A dotted Windows device basename was not sanitized");
    require(planner.reserve({tmp.path / "ab?c*.flac", tmp.path}).filename() == "ab_c_.mp3",
            "Windows-invalid output characters were not sanitized");
    test::throws<std::invalid_argument>([&] {
        (void)planner.reserve({tmp.path / "track.flac:stream", tmp.path});
    }, "Windows alternate data streams must not be treated as source files");
    require(planner.reserve({tmp.path / "trailing. .flac", tmp.path}).filename() ==
            "trailing.mp3", "Trailing Windows spaces and dots were not removed");
#else
    require(planner.reserve({tmp.path / "CON.flac", tmp.path}).filename() == "CON.mp3",
            "Linux filenames must not receive Windows-only sanitization");
#endif
}

} // namespace

int main(int argc, char** argv) {
    return test::run(argc, argv, {
        {"naming_collisions", test_collision_and_reservations},
        {"naming_source_directories", test_source_directories_and_mp3_inputs},
        {"naming_invalid_destination", test_invalid_destination}, {"naming_clear", test_clear},
        {"naming_gaps", test_gaps}, {"naming_relative", test_relative},
        {"naming_unicode", test_unicode}, {"naming_long_name", test_long_name},
        {"naming_alias", test_alias}, {"naming_occupied_links", test_occupied_links},
        {"naming_invalid_sources", test_invalid_sources}, {"naming_case_rules", test_case_rules},
        {"naming_windows_sanitization", test_windows_sanitization}
    });
}
