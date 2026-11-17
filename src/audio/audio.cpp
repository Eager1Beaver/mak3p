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
