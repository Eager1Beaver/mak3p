#include "audio/audio.hpp"
#include "audio/metadata.hpp"
#include "audio/process.hpp"
#include "test_support.hpp"

#include <nlohmann/json.hpp>
#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/id3v2header.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/tdebuglistener.h>
#include <taglib/tfilestream.h>

#include <algorithm>
#include <array>
#include <cmath>

using namespace audio_converter;
using test::require;
using test::throws;
using test::TempDirectory;

namespace {

fs::path fixtures;
fs::path helper;
fs::path ffmpeg;
fs::path ffprobe;
fs::path cli;

AudioProcessor processor() { return AudioProcessor({ffmpeg, ffprobe}); }

void completed(const ConversionResult& result) {
    require(result.state == JobState::Completed && result.output.has_value(),
            message(result.diagnostic) + " " + result.diagnostic.technical_detail);
}

fs::path copy_fixture(const std::string& name, const fs::path& destination) {
    fs::copy_file(fixtures / name, file_io_path(destination));
    return destination;
}

fs::path fake(const fs::path& directory, const std::string& name) {
    fs::path target = directory / name;
#ifdef _WIN32
    target += ".exe";
#endif
    fs::copy_file(helper, target);
    return target;
}

void check_properties(const fs::path& output, int bitrate, int channels, int rate) {
    const auto result = run_process({ffprobe, {"-v", "error", "-select_streams", "a",
        "-show_entries", "stream=codec_name,channels,sample_rate,bit_rate", "-of", "json",
        path_utf8(file_io_path(output))}});
    require(result.exit_code == 0, result.standard_error);
    const auto streams = nlohmann::json::parse(result.standard_output).at("streams");
    require(streams.size() == 1 && streams[0].at("codec_name") == "mp3" &&
            streams[0].at("channels") == channels &&
            streams[0].at("sample_rate") == std::to_string(rate) &&
            streams[0].at("bit_rate") == std::to_string(bitrate * 1000),
            "Independent ffprobe output does not match requested audio settings");
}

void policy() {
    for (int bitrate : {128, 160, 192, 224, 256, 320}) validate_settings({bitrate});
    throws<AudioError>([] { validate_settings({127}); }, "Unsupported bitrate accepted");
    throws<AudioError>([] { validate_settings({256, static_cast<ChannelMode>(99)}); },
                        "Invalid channel mode accepted");
    require(output_channels(1, ChannelMode::Auto) == 1 &&
            output_channels(2, ChannelMode::Auto) == 2 &&
            output_channels(6, ChannelMode::Auto) == 2 &&
            output_channels(1, ChannelMode::Stereo) == 2 &&
            output_channels(6, ChannelMode::Mono) == 1, "Channel policy is incorrect");
    for (int rate : {11025, 22050, 44100, 88200, 176400, 2822400}) {
        require(output_sample_rate(rate) == 44100, "44.1-kHz/DSD family rate was misclassified");
    }
    for (int rate : {8000, 12000, 16000, 24000, 32000, 48000, 96000, 192000}) {
        require(output_sample_rate(rate) == 48000, "48-kHz family rate was misclassified");
    }
    require(output_sample_rate(40000) == 44100 && output_sample_rate(47000) == 48000 &&
            output_sample_rate(46050) == 48000, "Unusual sample-rate selection is not deterministic");
    throws<std::invalid_argument>([] { (void)output_sample_rate(0); }, "Zero source rate accepted");
}

void probe_invalid() {
    auto engine = processor();
    for (const auto& path : {fixtures / "invalid.mp3", fixtures / "missing.flac", fixtures}) {
        const auto input = engine.inspect({path});
        require(input.state == JobState::UnsupportedSkipped && !input.diagnostic.key.empty(),
                "Invalid inputs must remain classifiable individually");
    }
}

void probe_disguised() {
    TempDirectory tmp;
    auto source = copy_fixture("stereo.flac", tmp.path / "actually-audio.unknown");
    const auto input = processor().inspect({source}, {256, ChannelMode::Auto, false, true});
    require(input.state == JobState::Ready && input.media && input.media->audio.codec == "flac",
            "Unknown extensions must not prevent content recognition");
    source = copy_fixture("cbr.mp3", tmp.path / "not-really-flac.flac");
    const auto mp3 = processor().inspect({source}, {256});
    require(mp3.state == JobState::SameBitrateSkipped && mp3.media &&
            mp3.media->audio.codec == "mp3", "Misleading extensions changed MP3 decisions");
}

void probe_image_fallback() {
    struct DebugCapture : TagLib::DebugListener {
        std::string messages;
        void printMessage(const TagLib::String& value) override { messages += value.to8Bit(true); }
        DebugCapture() { TagLib::setDebugListener(this); }
        ~DebugCapture() override { TagLib::setDebugListener(nullptr); }
    } debug;

    TempDirectory tmp;
    auto engine = processor();
    for (const auto* name : {"cover.png", "cover.jpg"}) {
        const auto source = copy_fixture(name, tmp.path / (std::string(name) + ".mp3"));
        require(engine.inspect({source}).diagnostic.key == "no_audio", "Image was accepted as audio");
    }
    require(debug.messages.empty(), "Image inspection emitted TagLib debug messages: " + debug.messages);
    const auto renamed = engine.inspect({copy_fixture("cbr.mp3", tmp.path / "audio.jpg")});
    require(renamed.media && renamed.media->mp3.mode == Mp3BitrateMode::Cbr,
            "Renamed MP3 was not recognized");
    const auto short_mp3 = copy_fixture("short.mp3", tmp.path / "short-audio.jpg");
    const auto inspected = engine.inspect({short_mp3});
    require(inspected.media && inspected.media->mp3.mode == Mp3BitrateMode::Cbr,
            "Short MP3 with an image extension was not recognized");
}

void probe_streams() {
    auto engine = processor();
    const auto multiple = engine.inspect({fixtures / "multiple.mka"}, {256, ChannelMode::Auto, false, true});
    const auto video = engine.inspect({fixtures / "video.mp4"}, {256, ChannelMode::Auto, false, true});
    require(multiple.state == JobState::Ready && multiple.media->audio.index == 0 &&
            multiple.media->audio.sample_rate == 44100, "The first usable audio stream was not selected");
    require(video.state == JobState::Ready && video.media->audio.index == 1 &&
            video.media->audio.codec == "aac" && video.media->audio.sample_rate == 44100,
            "The first audio stream in the video container was not selected");
}

void probe_stream_fallback() {
    TempDirectory tmp;
    AudioProcessor engine({ffmpeg, fake(tmp.path, "ffprobe-streams")});
    const auto input = engine.inspect({fixtures / "mono.wav"});
    require(input.state == JobState::Ready && input.media->audio.index == 0,
            "An unusable first declared stream prevented selection of a usable one");
}

void engine_errors() {
    throws<AudioError>([&] { AudioProcessor engine({"missing-ffmpeg-882f", ffprobe}); },
                       "Missing FFmpeg did not produce an actionable error");
    TempDirectory tmp;
    try {
        AudioProcessor engine({fake(tmp.path, "ffmpeg-no-lame"), ffprobe});
        throw std::runtime_error("Missing LAME was accepted");
    } catch (const AudioError& error) {
        require(error.diagnostic().key == "missing_encoder", "Wrong missing-encoder diagnosis");
    }
    AudioProcessor invalid({ffmpeg, fake(tmp.path, "ffprobe-invalid")});
    const auto input = invalid.inspect({fixtures / "mono.wav"});
    require(input.state == JobState::UnsupportedSkipped && input.diagnostic.key == "probe_invalid",
            "Invalid JSON was not isolated to the input");
}

void mp3_decisions() {
    auto engine = processor();
    const auto cbr = engine.inspect({fixtures / "cbr.mp3"}, {256});
    const auto vbr = engine.inspect({fixtures / "vbr.mp3"});
    require(cbr.state == JobState::SameBitrateSkipped && cbr.media->mp3.mode == Mp3BitrateMode::Cbr &&
            cbr.media->mp3.bitrate_kbps == 256, "Matching CBR was not verified and skipped");
    require(vbr.state == JobState::Ready && vbr.media->mp3.mode == Mp3BitrateMode::Vbr,
            "VBR was incorrectly classified from average bitrate");
    AudioSettings mono;
    mono.bitrate_kbps = 256;
    mono.channel_mode = ChannelMode::Mono;
    require(engine.inspect({fixtures / "cbr.mp3"}, mono).state == JobState::Ready,
            "Matching bitrate prevented a requested channel transformation");
    require(engine.inspect({fixtures / "cbr.mp3", ImportOrigin::FolderScan}).diagnostic.key ==
            "mp3_excluded", "Folder-import MP3 exclusion was ignored");
    AudioSettings include;
    include.bitrate_kbps = 256;
    include.include_mp3_from_folders = true;
    require(engine.inspect({fixtures / "cbr.mp3", ImportOrigin::FolderScan}, include).state ==
            JobState::SameBitrateSkipped, "Explicit folder MP3 inclusion was ignored");
}

void mp3_uncertain() {
    TempDirectory tmp;
    const auto broken = copy_fixture("cbr.mp3", tmp.path / "truncated.mp3");
    fs::resize_file(broken, fs::file_size(broken) - 150);
    require(analyze_mp3(broken).mode != Mp3BitrateMode::Cbr,
            "A truncated final MPEG frame was incorrectly accepted as verified CBR");
    test::write(tmp.path / "unknown.mp3", "not mpeg");
    require(analyze_mp3(tmp.path / "unknown.mp3").mode == Mp3BitrateMode::Unknown,
            "Unrecognized content was assigned a bitrate mode");
}

void mp3_bounded_analysis() {
    TempDirectory tmp;
    const auto large = copy_fixture("cbr.mp3", tmp.path / "large.mp3");
    fs::resize_file(large, 257ULL * 1024 * 1024);
    const auto start = std::chrono::steady_clock::now();
    require(analyze_mp3(large).mode == Mp3BitrateMode::Unknown &&
            std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
            "Bounded analysis must report uncertainty instead of scanning a very large input");
}

void convert_one(const std::string& fixture, AudioSettings settings, int channels, int rate) {
    settings.force_upsampling = true;
    TempDirectory tmp;
    const auto source = copy_fixture(fixture, tmp.path / fixture);
    auto engine = processor();
    const auto input = engine.inspect({source}, settings);
    require(input.state == JobState::Ready, message(input.diagnostic));
    OutputPathPlanner planner;
    OutputTransaction output(planner, {source, std::nullopt});
    const auto result = engine.convert(input, settings, output);
    completed(result);
    check_properties(*result.output, settings.bitrate_kbps, channels, rate);
}

void bitrates() {
    for (int bitrate : {128, 160, 192, 224, 256, 320}) {
        convert_one("mono.wav", {bitrate, ChannelMode::Auto}, 1, 44100);
    }
}

void channel_modes() {
    convert_one("stereo.flac", {}, 2, 48000);
    convert_one("stereo.flac", {256, ChannelMode::Mono}, 1, 48000);
    convert_one("mono.wav", {256, ChannelMode::Stereo}, 2, 44100);
    convert_one("tone.ac3", {}, 2, 48000);
    convert_one("tone.ac3", {256, ChannelMode::Mono}, 1, 48000);
}

void resampling() {
    convert_one("high.wav", {}, 2, 44100);
    convert_one("low.wav", {320, ChannelMode::Auto}, 1, 48000);
    convert_one("unusual.wav", {320, ChannelMode::Auto}, 1, 44100);
}

void formats() {
    for (const auto& name : {"tone.wv", "tone.aiff", "tone.m4a", "tone.ogg", "tone.opus",
                             "tone.wma", "tone.ac3", "cbr.mp3", "vbr.mp3", "video.mp4"}) {
        const bool stereo = std::string(name) == "cbr.mp3" || std::string(name) == "vbr.mp3" ||
                            std::string(name) == "tone.ac3";
        const bool rate48 = std::string(name) == "tone.ac3" || std::string(name) == "tone.opus";
        convert_one(name, {192, ChannelMode::Auto}, stereo ? 2 : 1, rate48 ? 48000 : 44100);
    }
}

void conversion_protection() {
    TempDirectory tmp;
    const auto source = copy_fixture("cbr.mp3", tmp.path / "song.mp3");
    const auto original = test::read(source);
    test::write(tmp.path / "song (1).mp3", "pre-existing");
    auto engine = processor();
    AudioSettings settings{192};
    const auto input = engine.inspect({source}, settings);
    OutputPathPlanner planner;
    OutputTransaction output(planner, {source, std::nullopt});
    require(output.final_path().filename() == "song (2).mp3", "Initial collision naming is incorrect");
    test::write(output.final_path(), "late conflict");
    const auto result = engine.convert(input, settings, output);
    completed(result);
    require(result.output->filename() == "song (3).mp3" && test::read(source) == original &&
            test::read(tmp.path / "song (1).mp3") == "pre-existing" &&
            test::read(tmp.path / "song (2).mp3") == "late conflict",
            "Conversion modified a source or existing/late output");
}

void stale_inspection() {
    TempDirectory tmp;
    const auto source = copy_fixture("mono.wav", tmp.path / "song.wav");
    auto engine = processor();
    const auto input = engine.inspect({source});
    test::write(source, test::read(source) + "changed");
    OutputPathPlanner planner;
    OutputTransaction output(planner, {source, std::nullopt});
    const auto temporary = output.temporary_path();
    const auto result = engine.convert(input, {}, output);
    require(result.state == JobState::Failed && result.diagnostic.key == "source_changed" &&
            !fs::exists(temporary) && !fs::exists(output.final_path()),
            "Stale inspection was used or left output behind");
}

void settings_changed() {
    TempDirectory tmp;
    auto engine = processor();
    const auto input = engine.inspect({fixtures / "mono.wav"});
    OutputPathPlanner planner;
    OutputTransaction output(planner, {input.source.path, tmp.path});
    const auto result = engine.convert(input, {192}, output);
    require(result.state == JobState::Failed && result.diagnostic.key == "settings_changed" &&
            fs::is_empty(tmp.path), "A settings change was used without reclassification");
}

void failed_conversion() {
    TempDirectory tmp;
    auto engine = processor();
    const auto input = engine.inspect({fixtures / "mono.wav"});
    AudioProcessor failed({fake(tmp.path, "ffmpeg-fail"), ffprobe});
    OutputPathPlanner planner;
    OutputTransaction output(planner, {input.source.path, tmp.path});
    const auto result = failed.convert(input, {}, output);
    require(result.state == JobState::Failed && result.diagnostic.key == "encode_failed" &&
            !fs::exists(output.temporary_path()) && !fs::exists(output.final_path()),
            "Encoder failure was not isolated and cleaned up");
}

void validation_failure() {
    TempDirectory tmp;
    const auto input = processor().inspect({fixtures / "mono.wav"});
    AudioProcessor invalid({ffmpeg, fake(tmp.path, "ffprobe-wrong-output")});
    OutputPathPlanner planner;
    OutputTransaction output(planner, {input.source.path, tmp.path});
    const auto result = invalid.convert(input, {}, output);
    require(result.state == JobState::Failed && result.diagnostic.key == "output_invalid" &&
            !fs::exists(output.temporary_path()) && !fs::exists(output.final_path()),
            "Unexpected output properties were published");
}

void conversion_cancellation() {
    TempDirectory tmp;
    auto engine = processor();
    const auto input = engine.inspect({fixtures / "mono.wav"});
    OutputPathPlanner planner;
    std::stop_source stop;
    OutputTransaction output(planner, {input.source.path, tmp.path});
    const auto result = engine.convert(input, {}, output, stop.get_token(),
        [&](const ProgressUpdate& update) {
            if (update.audio_time.count() > 0) stop.request_stop();
        });
    require(stop.stop_requested() && result.state == JobState::Cancelled && fs::is_empty(tmp.path),
            "Cancellation during reported conversion did not clean up");
}

void progress() {
    TempDirectory tmp;
    auto engine = processor();
    const auto input = engine.inspect({fixtures / "mono.wav"});
    OutputPathPlanner planner;
    OutputTransaction output(planner, {input.source.path, tmp.path});
    std::vector<ProgressUpdate> events;
    const auto result = engine.convert(input, {}, output, {}, [&](const ProgressUpdate& update) {
        if (update.fraction == 1.0) require(fs::exists(output.final_path()),
                                          "Completion progress preceded publication");
        events.push_back(update);
    });
    completed(result);
    require(events.size() >= 3 && events.front().fraction == 0.0 &&
            events.back().fraction == 1.0, "Per-file progress endpoints are missing");
    for (const auto& event : events) {
        require(!event.fraction || (*event.fraction >= 0 && *event.fraction <= 1),
                "Progress escaped its permitted range");
    }
}

void unknown_progress() {
    TempDirectory tmp;
    auto engine = processor();
    auto input = engine.inspect({fixtures / "mono.wav"});
    input.media->duration_seconds.reset();
    OutputPathPlanner planner;
    OutputTransaction output(planner, {input.source.path, tmp.path});
    std::vector<ProgressUpdate> events;
    const auto result = engine.convert(input, {}, output, {}, [&](const ProgressUpdate& update) {
        events.push_back(update);
    });
    completed(result);
    require(events.size() >= 2 && !events.front().fraction && events.back().fraction == 1.0,
            "Unknown duration must keep progress indeterminate until publication");
    for (std::size_t index = 0; index + 1 < events.size(); ++index) {
        require(!events[index].fraction, "A percentage was invented for unknown duration");
    }
}

void replaced_temporary() {
    TempDirectory tmp;
    auto engine = processor();
    const auto input = engine.inspect({fixtures / "mono.wav"});
    OutputPathPlanner planner;
    OutputTransaction output(planner, {input.source.path, tmp.path});
    const auto temporary = output.temporary_path();
    fs::remove(temporary);
    test::write(temporary, "foreign replacement");
    const auto result = engine.convert(input, {}, output);
    require(result.state == JobState::Failed && test::read(temporary) == "foreign replacement" &&
            !fs::exists(output.final_path()), "Encoder overwrote a detected foreign temporary file");
}

void unicode_conversion() {
    TempDirectory tmp;
    const auto folder = tmp.path / fs::path(u8"日本語 音楽");
    fs::create_directory(folder);
    const auto source = copy_fixture("stereo.flac", folder / fs::path(u8"Étude $(literal);.flac"));
    auto engine = processor();
    AudioSettings settings; settings.force_upsampling = true;
    const auto input = engine.inspect({source}, settings);
    require(input.state == JobState::Ready, message(input.diagnostic));
    OutputPathPlanner planner;
    OutputTransaction output(planner, {source, std::nullopt});
    const auto result = engine.convert(input, settings, output);
    completed(result);
    require(result.output->filename() == fs::path(u8"Étude $(literal);.mp3"),
            "Unicode or metacharacter path was modified");
}

TagLib::String text(const std::string& value) { return TagLib::String(value, TagLib::String::UTF8); }

void seed(const fs::path& source, const TagLib::PropertyMap& properties,
          const TagLib::List<TagLib::VariantMap>& pictures = {}) {
    const auto path = file_io_path(source);
    TagLib::FileRef file(path.c_str(), false);
    require(!file.isNull() && file.setProperties(properties).isEmpty() &&
            file.setComplexProperties("PICTURE", pictures) && file.save(),
            "Cannot prepare metadata fixture");
}

TagLib::PropertyMap properties(const fs::path& source) {
    const auto path = file_io_path(source);
    TagLib::FileStream stream(path.c_str(), true);
    TagLib::FileRef file(&stream, false);
    require(!file.isNull(), "Cannot read fixture metadata");
    return file.properties();
}

ConversionResult convert_metadata(const fs::path& source, const fs::path& destination) {
    auto engine = processor();
    AudioSettings settings; settings.force_upsampling = true;
    const auto input = engine.inspect({source}, settings);
    require(input.state == JobState::Ready, message(input.diagnostic));
    OutputPathPlanner planner;
    OutputTransaction output(planner, {source, destination});
    auto result = engine.convert(input, settings, output);
    completed(result);
    return result;
}

#ifdef _WIN32
void long_windows_paths() {
    TempDirectory tmp;
    const auto directory = tmp.path / std::string(110, 'a') / std::string(110, 'b');
    fs::create_directories(file_io_path(directory));
    const auto source = copy_fixture("stereo.flac", directory / fs::path(u8"Étude 日本語.flac"));
    const auto occupied = directory / fs::path(u8"Étude 日本語.mp3");
    require(directory.native().size() > 260, "Fixture directory must exceed MAX_PATH");
    test::write(occupied, "existing output");
    TagLib::PropertyMap fields;
    fields.insert("TITLE", text("Long Windows path"));
    seed(source, fields);
    const auto original = test::read(source);

    const auto result = convert_metadata(source, directory);
    require(*result.output == directory / fs::path(u8"Étude 日本語 (1).mp3"),
            "Long-path output did not preserve the basename and collision suffix");
    require(test::read(source) == original && test::read(occupied) == "existing output",
            "Conversion changed its source or an existing destination");
    require(properties(*result.output).value("TITLE") == fields.value("TITLE"),
            "Long-path conversion did not preserve its title");
    const auto analysis = analyze_mp3(*result.output);
    require(analysis.mode == Mp3BitrateMode::Cbr, "Long-path output was not readable as CBR MP3");
}
#endif

void metadata_text() {
    TempDirectory tmp;
    const auto source = copy_fixture("stereo.flac", tmp.path / "tags.flac");
    TagLib::PropertyMap expected;
    expected.insert("TITLE", text("Étude 日本語"));
    expected.insert("ARTIST", TagLib::StringList{text("First artist"), text("第二")});
    expected.insert("ALBUM", text("Album"));
    expected.insert("ALBUMARTIST", text("Ensemble"));
    expected.insert("TRACKNUMBER", text("3/12"));
    expected.insert("DISCNUMBER", text("1/2"));
    expected.insert("DATE", text("2026-11-17T13:14:15"));
    expected.insert("COMPOSER", text("Composer"));
    expected.insert("COMMENT", text("Comment with / and Unicode 音"));
    expected.insert("REPLAYGAIN_TRACK_GAIN", text("+1.25 dB"));
    expected.insert("MUSICBRAINZ_TRACKID", text("b07d6a3c-13e1-4d47-9120-91dd86c5cf83"));
    seed(source, expected);
    const auto original = test::read(source);
    const auto result = convert_metadata(source, tmp.path);
    const auto actual = properties(*result.output);
    for (const auto& [key, values] : expected) {
        if (key == "DATE") {
            require(actual.value(key) == TagLib::StringList(text("2026-11-17 13:14:15")),
                    "The date or timestamp precision changed");
            continue;
        }
        require(actual.value(key) == values, "Mapped text metadata changed: " + key.to8Bit(true));
    }
    require(std::any_of(result.warnings.begin(), result.warnings.end(),
                        [](const Diagnostic& diagnostic) { return diagnostic.key == "metadata_v4"; }),
            "An ID3 version upgrade must be reported");
    require(test::read(source) == original, "Source tags were modified during conversion");
}

TagLib::VariantMap picture(const std::string& image, const std::string& mime,
                           const std::string& role, const std::string& description) {
    const auto bytes = test::read(fixtures / image);
    TagLib::VariantMap result;
    result.insert("data", TagLib::ByteVector(bytes.data(), static_cast<unsigned>(bytes.size())));
    result.insert("mimeType", text(mime));
    result.insert("pictureType", text(role));
    result.insert("description", text(description));
    return result;
}

void metadata_artwork() {
    TempDirectory tmp;
    const auto source = copy_fixture("stereo.flac", tmp.path / "covers.flac");
    TagLib::List<TagLib::VariantMap> expected;
    expected.append(picture("cover.png", "image/png", "Front Cover", "表紙"));
    expected.append(picture("cover.jpg", "image/jpeg", "Back Cover", "Back"));
    seed(source, {}, expected);
    const auto result = convert_metadata(source, tmp.path);
    TagLib::FileRef file(result.output->c_str(), false);
    const auto actual = file.complexProperties("PICTURE");
    require(actual.size() == 2, "Embedded artwork count changed");
    for (unsigned index = 0; index < actual.size(); ++index) {
        for (const auto& key : {"data", "description", "pictureType", "mimeType"}) {
            require(actual[index].value(key) == expected[index].value(key),
                    "Artwork bytes, role, type, or description changed");
        }
    }
}

void metadata_totals() {
    TempDirectory tmp;
    const auto source = copy_fixture("stereo.flac", tmp.path / "totals.flac");
    TagLib::PropertyMap fields;
    fields.insert("TRACKNUMBER", text("3"));
    fields.insert("TRACKTOTAL", text("12"));
    fields.insert("DISCNUMBER", text("1"));
    fields.insert("TOTALDISCS", text("2"));
    seed(source, fields);
    const auto result = convert_metadata(source, tmp.path);
    const auto actual = properties(*result.output);
    require(actual.value("TRACKNUMBER") == TagLib::StringList(text("3/12")) &&
            actual.value("DISCNUMBER") == TagLib::StringList(text("1/2")),
            "Separate track/disc totals were not mapped to their ID3 representation");
}

void metadata_omissions() {
    TempDirectory tmp;
    const auto source = copy_fixture("stereo.flac", tmp.path / "unknown.flac");
    TagLib::PropertyMap fields;
    fields.insert("TITLE", text("Retained"));
    fields.insert("SOURCE_CODEC_PRIVATE_THING", text("source only"));
    TagLib::List<TagLib::VariantMap> pictures;
    pictures.append(picture("cover.png", "image/bmp", "Front Cover", ""));
    seed(source, fields, pictures);
    const auto original = test::read(source);
    const auto result = convert_metadata(source, tmp.path);
    const auto has_warning = [&](const std::string& key) {
        return std::any_of(result.warnings.begin(), result.warnings.end(),
                           [&](const Diagnostic& warning) { return warning.key == key; });
    };
    require(has_warning("metadata_unmapped") && has_warning("metadata_picture_unsupported") &&
            properties(*result.output).value("TITLE") == TagLib::StringList(text("Retained")) &&
            !properties(*result.output).contains("SOURCE_CODEC_PRIVATE_THING") &&
            test::read(source) == original, "Unmapped metadata was lost from the source or not reported");
}

void metadata_absent() {
    TempDirectory tmp;
    const auto source = copy_fixture("stereo.flac", tmp.path / "no-tags.flac");
    seed(source, {});
    const auto result = convert_metadata(source, tmp.path);
    require(properties(*result.output).isEmpty(), "Absent metadata values were invented");
    TagLib::MPEG::File file(result.output->c_str(), false);
    require(!file.hasID3v2Tag() || file.ID3v2Tag()->header()->majorVersion() == 3,
            "Ordinary metadata should use ID3v2.3 compatibility");
}

void metadata_containers() {
    TempDirectory tmp;
    for (const auto& name : {"tone.wv", "tone.m4a", "tone.ogg", "tone.opus", "tone.wma", "mono.wav", "tone.aiff"}) {
        const auto source = copy_fixture(name, tmp.path / name);
        TagLib::PropertyMap fields;
        fields.insert("TITLE", text("Container 日本語"));
        fields.insert("ARTIST", text("Artist"));
        seed(source, fields);
        const auto result = convert_metadata(source, tmp.path);
        require(properties(*result.output).value("TITLE") == fields.value("TITLE") &&
                properties(*result.output).value("ARTIST") == fields.value("ARTIST"),
                std::string("Metadata was not preserved from ") + name);
    }
}

void readonly_source() {
    TempDirectory tmp;
    const auto source = copy_fixture("stereo.flac", tmp.path / "read-only.flac");
    TagLib::PropertyMap fields;
    fields.insert("TITLE", text("Read only"));
    seed(source, fields);
    const auto original = test::read(source);
    fs::permissions(source, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
    const auto result = convert_metadata(source, tmp.path);
    require(test::read(source) == original && properties(*result.output).value("TITLE") == fields.value("TITLE"),
            "Read-only source conversion modified or lost metadata");
}

void cli_smoke() {
    const auto result = run_process({cli, {"inspect", path_utf8(fixtures / "mono.wav")}});
    require(result.exit_code == 0 && result.standard_output.find("Ready.") != std::string::npos,
            "The command-line inspection path failed");
    const auto invalid = run_process({cli, {"inspect", path_utf8(fixtures / "mono.wav"), "--bitrate", "17"}});
    require(invalid.exit_code != 0 && invalid.standard_error.find("128") != std::string::npos,
            "Command-line validation did not explain allowed bitrates");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 7) return 2;
    fixtures = argv[2];
    helper = argv[3];
    ffmpeg = argv[4];
    ffprobe = argv[5];
    cli = argv[6];
    return test::run(argc, argv, {
        {"policy", policy}, {"probe_invalid", probe_invalid}, {"probe_disguised", probe_disguised},
        {"probe_image_fallback", probe_image_fallback}, {"probe_streams", probe_streams},
        {"probe_stream_fallback", probe_stream_fallback},
        {"engine_errors", engine_errors}, {"mp3_decisions", mp3_decisions},
        {"mp3_uncertain", mp3_uncertain}, {"mp3_bounded_analysis", mp3_bounded_analysis},
        {"bitrates", bitrates}, {"channel_modes", channel_modes},
        {"resampling", resampling}, {"formats", formats}, {"conversion_protection", conversion_protection},
        {"stale_inspection", stale_inspection}, {"settings_changed", settings_changed},
        {"failed_conversion", failed_conversion}, {"validation_failure", validation_failure},
        {"conversion_cancellation", conversion_cancellation}, {"progress", progress},
        {"unknown_progress", unknown_progress},
        {"replaced_temporary", replaced_temporary}, {"unicode_conversion", unicode_conversion},
#ifdef _WIN32
        {"long_windows_paths", long_windows_paths},
#endif
        {"metadata_text", metadata_text}, {"metadata_artwork", metadata_artwork},
        {"metadata_totals", metadata_totals},
        {"metadata_omissions", metadata_omissions}, {"metadata_absent", metadata_absent},
        {"metadata_containers", metadata_containers}, {"readonly_source", readonly_source},
        {"cli_smoke", cli_smoke}
    });
}
