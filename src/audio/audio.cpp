#include "audio/audio.hpp"
#include "audio/metadata.hpp"
#include "audio/process.hpp"

#include <nlohmann/json.hpp>
#include <taglib/mpegfile.h>
#include <taglib/mpegheader.h>
#include <taglib/tfilestream.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <sstream>
#include <utility>

namespace audio_converter {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

std::string process_detail(const ProcessResult& result) {
    auto detail = result.standard_error;
    if (result.stderr_truncated) detail += "\n[diagnostics truncated]";
    return detail;
}

void checked(const ProcessResult& result, const std::string& failure) {
    if (result.cancelled) throw AudioError({"process_cancelled", {}, process_detail(result)});
    if (result.timed_out) throw AudioError({"process_timeout", {}, process_detail(result)});
    if (result.exit_code != 0) throw AudioError({failure, {}, process_detail(result)});
}

template<class Number>
std::optional<Number> number(const std::string& text) {
    Number value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    if constexpr (std::is_floating_point_v<Number>) {
        if (!std::isfinite(value)) return std::nullopt;
    }
    return value;
}

std::optional<double> duration(const Json& object) {
    if (!object.contains("duration")) return std::nullopt;
    const auto& value = object.at("duration");
    const auto parsed = value.is_string() ? number<double>(value.get<std::string>()) :
        value.is_number() ? std::optional<double>(value.get<double>()) : std::nullopt;
    return parsed && std::isfinite(*parsed) && *parsed > 0 ? parsed : std::nullopt;
}

MediaTags tags(const Json& object) {
    MediaTags result;
    if (object.contains("tags") && object.at("tags").is_object()) {
        for (const auto& [key, value] : object.at("tags").items()) {
            if (value.is_string()) result[key] = value.get<std::string>();
        }
    }
    return result;
}

bool format_is_mp3(const MediaInfo& media) {
    return media.audio.codec == "mp3" &&
           ("," + media.format + ",").find(",mp3,") != std::string::npos;
}

ProcessRequest decode_request(const fs::path& executable, const fs::path& source, int stream) {
    ProcessRequest request;
    request.executable = executable;
    request.arguments = {
        "-nostdin", "-hide_banner", "-v", "error", "-xerror", "-threads", "1",
        "-protocol_whitelist", "file,pipe", "-i", path_utf8(source),
        "-map", "0:" + std::to_string(stream), "-vn", "-sn", "-dn", "-f", "null", "-"
    };
    return request;
}

void source_unchanged(const Inspection& input) {
    std::error_code size_error;
    const auto size = fs::file_size(input.source.path, size_error);
    std::error_code time_error;
    const auto time = fs::last_write_time(input.source.path, time_error);
    if (size_error || time_error || size != input.source_size || time != input.source_modified) {
        throw AudioError({"source_changed", {}, {}});
    }
}

class ProgressReader {
public:
    ProgressReader(std::optional<double> duration_seconds, ProgressCallback callback)
        : duration_(duration_seconds), callback_(std::move(callback)) {}

    void consume(std::string_view bytes) {
        if (!callback_) return;
        pending_.append(bytes);
        std::size_t newline = 0;
        while ((newline = pending_.find('\n')) != std::string::npos) {
            auto line = pending_.substr(0, newline);
            pending_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            constexpr std::string_view prefix = "out_time_us=";
            if (line.starts_with(prefix)) {
                if (const auto value = number<long long>(line.substr(prefix.size()))) {
                    ProgressUpdate update;
                    update.audio_time = std::chrono::microseconds(std::max(0LL, *value));
                    if (duration_) {
                        update.fraction = std::clamp(
                            static_cast<double>(update.audio_time.count()) / 1'000'000.0 / *duration_,
                            0.0, 0.99);
                    }
                    callback_(update);
                }
            }
        }
        if (pending_.size() > 65536) {
            throw AudioError({"probe_invalid", {}, "Progress record exceeded its size limit"});
        }
    }

private:
    std::optional<double> duration_;
    ProgressCallback callback_;
    std::string pending_;
};

} // namespace

void validate_settings(const AudioSettings& settings) {
    constexpr std::array rates{128, 160, 192, 224, 256, 320};
    if (std::find(rates.begin(), rates.end(), settings.bitrate_kbps) == rates.end()) {
        throw AudioError({"invalid_bitrate", {}, {}});
    }
    if (settings.channel_mode != ChannelMode::Auto &&
        settings.channel_mode != ChannelMode::Stereo && settings.channel_mode != ChannelMode::Mono) {
        throw AudioError({"invalid_channels", {}, {}});
    }
}

int output_channels(int source_channels, ChannelMode mode) {
    if (source_channels < 1) throw std::invalid_argument("Source channel count must be positive");
    switch (mode) {
    case ChannelMode::Auto: return source_channels == 1 ? 1 : 2;
    case ChannelMode::Stereo: return 2;
    case ChannelMode::Mono: return 1;
    }
    throw AudioError({"invalid_channels", {}, {}});
}

int output_sample_rate(int source_rate) {
    if (source_rate < 1) throw std::invalid_argument("Source sample rate must be positive");
    const auto family = [&](int base) {
        const int larger = std::max(base, source_rate);
        const int smaller = std::min(base, source_rate);
        if (larger % smaller != 0) return false;
        const int ratio = larger / smaller;
        return (ratio & (ratio - 1)) == 0;
    };
    if (family(11025)) return 44100;
    if (family(8000) || family(12000)) {
        return 48000;
    }
    return std::abs(source_rate - 44100) < std::abs(source_rate - 48000) ? 44100 : 48000;
}

namespace {

enum class Mp3Scan { BoundedInput, CompleteOutput };

Mp3Analysis scan_mp3(const fs::path& source, std::stop_token stop, Mp3Scan scope) {
    const bool bounded = scope == Mp3Scan::BoundedInput;
    std::error_code size_error;
    const auto size = fs::file_size(source, size_error);
    if (size_error || (bounded && size > 256ULL * 1024 * 1024)) return {};
    TagLib::FileStream stream(source.c_str(), true);
    if (!stream.isOpen()) return {};
    TagLib::MPEG::File file(&stream, false);
    if (!file.isValid()) return {};
    const auto first = file.firstFrameOffset();
    const auto last = file.lastFrameOffset();
    if (first < 0 || last <= first ||
        (bounded && last - first > 256LL * 1024 * 1024)) return {};
    auto offset = first;
    int bitrate = 0;
    int sample_rate = 0;
    bool variable = false;
    double seconds = 0;
    for (std::uint64_t frames = 0; (!bounded || frames < 1'000'000) && offset <= last; ++frames) {
        if (stop.stop_requested()) throw AudioError({"process_cancelled", {}, {}});
        TagLib::MPEG::Header header(&file, offset, false);
        if (!header.isValid() || header.layer() != 3 || header.frameLength() < 4 ||
            header.bitrate() <= 0 || header.sampleRate() <= 0 ||
            offset + header.frameLength() > file.length()) return {};
        if (frames == 0) {
            bitrate = header.bitrate();
            sample_rate = header.sampleRate();
        }
        if (sample_rate != header.sampleRate()) return {};
        variable = variable || header.bitrate() != bitrate;
        seconds += static_cast<double>(header.samplesPerFrame()) / header.sampleRate();
        if (offset == last) {
            return {variable ? Mp3BitrateMode::Vbr : Mp3BitrateMode::Cbr,
                    variable ? std::nullopt : std::optional<int>(bitrate), seconds};
        }
        offset += header.frameLength();
    }
    return {};
}

} // namespace

Mp3Analysis analyze_mp3(const fs::path& source, std::stop_token stop) {
    return scan_mp3(source, stop, Mp3Scan::BoundedInput);
}

AudioProcessor::AudioProcessor(EnginePaths paths) : paths_(std::move(paths)) {
    try {
        paths_.ffmpeg = resolve_executable(paths_.ffmpeg);
        paths_.ffprobe = resolve_executable(paths_.ffprobe);
        auto ffmpeg = run_process({paths_.ffmpeg, {"-version"}, 10s});
        auto ffprobe = run_process({paths_.ffprobe, {"-version"}, 10s});
        checked(ffmpeg, "engine_probe_failed");
        checked(ffprobe, "engine_probe_failed");
        if (!ffmpeg.standard_output.starts_with("ffmpeg version ") ||
            !ffprobe.standard_output.starts_with("ffprobe version ")) {
            throw AudioError({"engine_probe_failed", {}, {}});
        }
        ffmpeg_version_ = ffmpeg.standard_output.substr(0, ffmpeg.standard_output.find('\n'));
        ffprobe_version_ = ffprobe.standard_output.substr(0, ffprobe.standard_output.find('\n'));
        auto encoders = run_process({paths_.ffmpeg, {"-hide_banner", "-encoders"}, 10s});
        checked(encoders, "engine_probe_failed");
        bool lame = false;
        std::istringstream lines(encoders.standard_output);
        for (std::string line; std::getline(lines, line);) {
            std::istringstream fields(line);
            std::string flags, name;
            fields >> flags >> name;
            if (name == "libmp3lame" && flags.starts_with('A')) lame = true;
        }
        if (!lame || encoders.stdout_truncated) throw AudioError({"missing_encoder", {}, {}});
    } catch (const AudioError&) {
        throw;
    } catch (const std::exception& error) {
        throw AudioError({"engine_missing", {}, error.what()});
    }
}

MediaInfo AudioProcessor::probe(const fs::path& source, 
                                bool check_decoding,
                                std::stop_token stop) const {
    ProcessRequest request{paths_.ffprobe, {
        "-v", "error", "-protocol_whitelist", "file,pipe",
        "-show_entries",
        "stream=index,codec_type,codec_name,sample_rate,channels,channel_layout,duration:"
        "stream_tags:format=format_name,duration:format_tags",
        "-of", "json", "-i", path_utf8(source)
    }, 30s};
    request.output_limit = 16 * 1024 * 1024;
    const auto result = run_process(request, stop);
    checked(result, "unsupported");
    if (result.stdout_truncated) throw AudioError({"probe_invalid", {}, {}});
    Json document;
    try {
        document = Json::parse(result.standard_output);
        if (!document.is_object() || !document.contains("streams") ||
            !document.at("streams").is_array() || !document.contains("format") ||
            !document.at("format").is_object()) {
            throw AudioError({"probe_invalid", {}, {}});
        }
        MediaInfo media;
        const auto& format = document.at("format");
        media.format = format.value("format_name", std::string());
        media.tags = tags(format);
        const auto format_duration = duration(format);
        std::string errors;
        for (const auto& entry : document.at("streams")) {
            if (entry.value("codec_type", std::string()) != "audio") continue;
            AudioStream audio;
            audio.index = entry.value("index", -1);
            audio.codec = entry.value("codec_name", std::string());
            audio.sample_rate = number<int>(entry.value("sample_rate", std::string())).value_or(0);
            audio.channels = entry.value("channels", 0);
            audio.channel_layout = entry.value("channel_layout", std::string());
            audio.duration_seconds = duration(entry);
            audio.tags = tags(entry);
            if (audio.index < 0 || audio.codec.empty() || audio.codec == "unknown" ||
                audio.sample_rate < 1 || audio.channels < 1) continue;
            if (check_decoding) {
                auto decode = decode_request(paths_.ffmpeg, source, audio.index);
                decode.arguments[decode.arguments.size() - 2] = "framehash";
                const auto end = decode.arguments.end() - 3;
                decode.arguments.insert(end, {"-frames:a", "2"});
                decode.timeout = 30s;
                const auto decoded = run_process(decode, stop);
                if (decoded.cancelled || decoded.timed_out) checked(decoded, "decode_failed");
                bool produced = false;
                std::istringstream frames(decoded.standard_output);
                for (std::string line; std::getline(frames, line);) {
                    if (line.starts_with("0,")) produced = true;
                }
                if (decoded.exit_code != 0 || !produced) {
                    errors += process_detail(decoded);
                    continue;
                }
            }
            media.audio = std::move(audio);
            media.duration_seconds = media.audio.duration_seconds ?
                media.audio.duration_seconds : format_duration;
            if (format_is_mp3(media)) {
                media.mp3 = scan_mp3(source, stop, check_decoding ?
                                     Mp3Scan::BoundedInput : Mp3Scan::CompleteOutput);
                if (media.mp3.duration_seconds) {
                    media.duration_seconds = media.mp3.duration_seconds;
                } else {
                    media.duration_seconds.reset();
                }
            }
            return media;
        }
        throw AudioError({"no_audio", {}, errors});
    } catch (const Json::exception& error) {
        throw AudioError({"probe_invalid", {}, error.what()});
    }
}

Inspection AudioProcessor::inspect(const SourceFile& source, 
                                   const AudioSettings& settings,
                                   std::stop_token stop) const {
    validate_settings(settings);
    Inspection inspection;
    inspection.source = source;
    inspection.settings = settings;
    try {
        if (stop.stop_requested()) throw AudioError({"process_cancelled", {}, {}});
        inspection.source.path = normalize_source_path(source.path);
        if (!fs::is_regular_file(inspection.source.path)) {
            throw AudioError({"source_unreadable", {}, {}});
        }
        inspection.source_size = fs::file_size(inspection.source.path);
        inspection.source_modified = fs::last_write_time(inspection.source.path);
        auto media = probe(inspection.source.path, true, stop);
        source_unchanged(inspection);
        const bool mp3 = format_is_mp3(media);
        inspection.media = std::move(media);
        if (mp3 && source.origin == ImportOrigin::FolderScan &&
            !settings.include_mp3_from_folders) {
            inspection.diagnostic = {"mp3_excluded", {}, {}};
            return inspection;
        }
        const auto& audio = inspection.media->audio;
        if (mp3 && inspection.media->mp3.mode == Mp3BitrateMode::Cbr &&
            inspection.media->mp3.bitrate_kbps == settings.bitrate_kbps &&
            output_channels(audio.channels, settings.channel_mode) == audio.channels) {
            inspection.state = JobState::SameBitrateSkipped;
            inspection.diagnostic = {"same_bitrate", {}, {}};
            return inspection;
        }
        inspection.state = JobState::Ready;
        inspection.diagnostic = {"ready", {}, {}};
    } catch (const AudioError& error) {
        inspection.diagnostic = error.diagnostic();
        if (inspection.diagnostic.key == "process_cancelled") inspection.state = JobState::Cancelled;
    } catch (const std::exception& error) {
        inspection.diagnostic = {"source_unreadable", {}, error.what()};
    }
    return inspection;
}

} // namespace audio_converter
