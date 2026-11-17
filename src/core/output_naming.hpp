#pragma once

#include "core/path_utils.hpp"

#include <mutex>
#include <optional>
#include <set>

namespace audio_converter {

// A missing destination_directory means "place the MP3 beside the source".
struct OutputRequest {
    fs::path source;
    std::optional<fs::path> destination_directory;
};

// One planner belongs to one batch. Reserve all output paths before launching
// parallel conversion jobs so two workers never choose the same filename.
//
// Reservations are in-memory only. They are NOT a filesystem lock: the final
// commit step must still use a no-overwrite operation.
class OutputPathPlanner {
public:
    // Returns e.g. /music/Track.mp3, Track (1).mp3, Track (2).mp3, ...
    // Returns an absolute path. Filesystem failures leave existing entries intact.
    [[nodiscard]] fs::path reserve(const OutputRequest& request);

    // Clear only after all jobs in the batch have stopped.
    void clear();

private:
    std::mutex mutex_;
    std::set<fs::path, PathLess> reserved_;
};

} // namespace audio_converter
