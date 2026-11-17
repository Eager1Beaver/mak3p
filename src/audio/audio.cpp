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

} // namespace audio_converter
