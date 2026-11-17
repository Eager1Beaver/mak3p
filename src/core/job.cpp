#include "core/job.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace audio_converter {

namespace {

void require_reason(const std::string& reason) {
    if (reason.empty()) {
        throw std::invalid_argument("A skipped or failed job needs a reason");
    }
}

} // namespace

ConversionJob::ConversionJob(JobId id, SourceFile source)
    : id_(id), source_{normalize_source_path(source.path), source.origin} {
    if (id == 0) {
        throw std::invalid_argument("Job identifiers must be positive");
    }
}

void ConversionJob::require_state(JobState expected) const {
    if (state_ != expected) {
        throw std::logic_error("Invalid job state transition");
    }
}

void ConversionJob::mark_ready() {
    require_state(JobState::Checking);
    state_ = JobState::Ready;
}

void ConversionJob::skip_same_bitrate(std::string reason) {
    require_state(JobState::Checking);
    require_reason(reason);
    detail_ = std::move(reason);
    state_ = JobState::SameBitrateSkipped;
}

void ConversionJob::skip_unsupported(std::string reason) {
    require_state(JobState::Checking);
    require_reason(reason);
    detail_ = std::move(reason);
    state_ = JobState::UnsupportedSkipped;
}

void ConversionJob::start() {
    require_state(JobState::Ready);
    state_ = JobState::Converting;
    progress_.reset();
}

void ConversionJob::complete(fs::path output) {
    require_state(JobState::Converting);
    output = normalize_source_path(output);
    output_ = std::move(output);
    state_ = JobState::Completed;
    progress_ = 1.0;
}

void ConversionJob::fail(std::string reason) {
    require_state(JobState::Converting);
    require_reason(reason);
    detail_ = std::move(reason);
    state_ = JobState::Failed;
}

void ConversionJob::cancel() {
    if (state_ != JobState::Checking && state_ != JobState::Ready &&
        state_ != JobState::Converting && state_ != JobState::Cancelled) {
        throw std::logic_error("Only unfinished jobs can be cancelled");
    }
    state_ = JobState::Cancelled;
}

void ConversionJob::reset() {
    if (state_ == JobState::Converting) {
        throw std::logic_error("A running job cannot be reset");
    }
    detail_.clear();
    progress_.reset();
    output_.reset();
    state_ = JobState::Checking;
}

void ConversionJob::update_progress(std::optional<double> fraction) {
    require_state(JobState::Converting);
    if (fraction && (!std::isfinite(*fraction) || *fraction < 0.0 || *fraction > 1.0)) {
        throw std::invalid_argument("Progress must be between zero and one");
    }
    progress_ = fraction;
}

QueueAddResult FileQueue::add(const fs::path& path, ImportOrigin origin) {
    const auto normalized = normalize_source_path(path);
    if (const auto found = indices_.find(normalized); found != indices_.end()) {
        auto& existing = jobs_[found->second];
        const bool promote = origin == ImportOrigin::SelectedFile &&
                             existing.source_.origin == ImportOrigin::FolderScan;
        if (promote) {
            existing.source_.origin = ImportOrigin::SelectedFile;
        }
        return {existing.id(), false, promote};
    }
    if (next_id_ == std::numeric_limits<JobId>::max()) {
        throw std::overflow_error("Job identifiers exhausted");
    }
    const auto [entry, inserted] = indices_.emplace(normalized, jobs_.size());
    (void)inserted;
    try {
        jobs_.emplace_back(next_id_, SourceFile{normalized, origin});
    } catch (...) {
        indices_.erase(entry);
        throw;
    }
    return {next_id_++, true};
}

ConversionJob& FileQueue::job(JobId id) {
    for (auto& entry : jobs_) {
        if (entry.id() == id) {
            return entry;
        }
    }
    throw std::out_of_range("Unknown job identifier");
}

void FileQueue::clear() noexcept {
    indices_.clear();
    jobs_.clear();
}

} // namespace audio_converter
