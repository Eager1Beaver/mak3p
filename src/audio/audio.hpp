#pragma once

#include "audio/diagnostic.hpp"
#include "core/job.hpp"
#include "core/output_transaction.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace audio_converter {

enum class ChannelMode { Auto, Stereo, Mono };

struct AudioSettings {
    int bitrate_kbps = 320;
    ChannelMode channel_mode = ChannelMode::Stereo;
    bool include_mp3_from_folders = false;
    bool force_upsampling = false;
    bool operator==(const AudioSettings&) const = default;
};

enum class Mp3BitrateMode { NotMp3, Cbr, Vbr, Unknown };

struct Mp3Analysis {
    Mp3BitrateMode mode = Mp3BitrateMode::Unknown;
    std::optional<int> bitrate_kbps;
    std::optional<double> duration_seconds;
};

using MediaTags = std::map<std::string, std::string>;

struct AudioStream {
    int index = -1;
    std::string codec;
    int sample_rate = 0;
    int channels = 0;
    std::string channel_layout;
    std::optional<double> duration_seconds;
    MediaTags tags;
    std::optional<std::uint64_t> bitrate_bps;
};

struct MediaInfo {
    std::string format;
    AudioStream audio;
    MediaTags tags;
    std::optional<double> duration_seconds;
    Mp3Analysis mp3{Mp3BitrateMode::NotMp3, std::nullopt, std::nullopt};
    std::optional<std::uint64_t> bitrate_bps;
    bool bitrate_estimated = false;
};

struct SourceInspection {
    SourceFile source;
    JobState state = JobState::UnsupportedSkipped;
    Diagnostic diagnostic;
    std::optional<MediaInfo> media;
    std::uintmax_t source_size = 0;
    fs::file_time_type source_modified{};
    std::vector<Diagnostic> warnings;
};

struct Inspection : SourceInspection {
    AudioSettings settings;
    std::optional<int> output_bitrate_kbps;
};

struct EnginePaths {
    fs::path ffmpeg = "ffmpeg";
    fs::path ffprobe = "ffprobe";
};

struct ProgressUpdate {
    std::optional<double> fraction;
    std::chrono::microseconds audio_time{0};
};

using ProgressCallback = std::function<void(const ProgressUpdate&)>;

struct ConversionResult {
    JobState state = JobState::Failed;
    Diagnostic diagnostic;
    std::optional<fs::path> output;
    std::vector<Diagnostic> warnings;
};

void validate_settings(const AudioSettings& settings);
// Chooses a standard CBR rate within the target and known source bitrate.
// No value means the source is below the minimum supported output rate.
[[nodiscard]] std::optional<int> select_output_bitrate(
    const AudioSettings& settings, std::optional<std::uint64_t> source_bitrate_bps);
[[nodiscard]] int output_channels(int source_channels, ChannelMode mode);
[[nodiscard]] int output_sample_rate(int source_rate);
[[nodiscard]] Mp3Analysis analyze_mp3(const fs::path& source, std::stop_token stop = {});
// Applies output policy to source observations without accessing the file or tools.
[[nodiscard]] Inspection classify_source(const SourceInspection& source, const AudioSettings& settings);

class AudioProcessor {
public:
    explicit AudioProcessor(EnginePaths paths = {}, std::stop_token stop = {});
    [[nodiscard]] const std::string& ffmpeg_version() const noexcept { return ffmpeg_version_; }
    [[nodiscard]] const std::string& ffprobe_version() const noexcept { return ffprobe_version_; }

    [[nodiscard]] SourceInspection inspect_source(const SourceFile& source, std::stop_token stop = {}) const;
    [[nodiscard]] Inspection inspect(const SourceFile& source, const AudioSettings& settings = {},
                                     std::stop_token stop = {}) const;
    [[nodiscard]] ConversionResult convert(const Inspection& input, const AudioSettings& settings,
                                          OutputTransaction& output, std::stop_token stop = {},
                                          ProgressCallback progress = {}) const;

private:
    EnginePaths paths_;
    std::string ffmpeg_version_;
    std::string ffprobe_version_;

    [[nodiscard]] MediaInfo probe(const fs::path& source, bool check_decoding,
                                  std::stop_token stop) const;
};

} // namespace audio_converter
