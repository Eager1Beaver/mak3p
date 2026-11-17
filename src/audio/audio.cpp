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

void source_unchanged(const Inspection& input) {
    std::error_code size_error;
    const auto size = fs::file_size(input.source.path, size_error);
    std::error_code time_error;
    const auto time = fs::last_write_time(input.source.path, time_error);
    if (size_error || time_error || size != input.source_size || time != input.source_modified) {
        throw AudioError({"source_changed", {}, {}});
    }
}


}

} // namespace audio_converter
