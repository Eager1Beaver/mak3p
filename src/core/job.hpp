#pragma once

#include "core/path_utils.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace audio_converter {

enum class ImportOrigin { SelectedFile, FolderScan };

struct SourceFile {
    fs::path path;
    ImportOrigin origin = ImportOrigin::SelectedFile;
};

enum class JobState {
    Checking,
    Ready,
    SameBitrateSkipped,
    LowerBitrateSkipped,
    UnsupportedSkipped,
    Converting,
    Completed,
    Failed,
    Cancelled
};

using JobId = std::uint64_t;

class ConversionJob {
public:
    ConversionJob(JobId id, SourceFile source);

    [[nodiscard]] JobId id() const noexcept { return id_; }
    [[nodiscard]] const SourceFile& source() const noexcept { return source_; }
    [[nodiscard]] JobState state() const noexcept { return state_; }
    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }
    [[nodiscard]] std::optional<double> progress() const noexcept { return progress_; }
    [[nodiscard]] const std::optional<fs::path>& output() const noexcept { return output_; }

    void mark_ready();
    void skip_same_bitrate(std::string reason);
    void skip_lower_bitrate(std::string reason);
    void skip_unsupported(std::string reason);
    void start();
    void complete(fs::path output);
    void fail(std::string reason);
    void cancel();
    void reset();
    void update_progress(std::optional<double> fraction);

private:
    friend class FileQueue;

    JobId id_;
    SourceFile source_;
    JobState state_ = JobState::Checking;
    std::string detail_;
    std::optional<double> progress_;
    std::optional<fs::path> output_;

    void require_state(JobState expected) const;
};

struct QueueAddResult {
    JobId id;
    bool added;
    bool origin_changed = false;
};

// The controller owns queue mutations and synchronizes access to job objects.
class FileQueue {
public:
    [[nodiscard]] QueueAddResult add(const fs::path& path, ImportOrigin origin);
    [[nodiscard]] std::span<const ConversionJob> jobs() const noexcept { return jobs_; }
    [[nodiscard]] ConversionJob& job(JobId id);
    [[nodiscard]] bool remove(JobId id);
    void clear() noexcept;

private:
    std::vector<ConversionJob> jobs_;
    std::map<fs::path, std::size_t, PathLess> indices_;
    JobId next_id_ = 1;
};

} // namespace audio_converter
