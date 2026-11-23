#include "core/job.hpp"
#include "test_support.hpp"

#include <limits>

using namespace audio_converter;
using test::require;
using test::TempDirectory;
using test::throws;

namespace {

void queue_normalization() {
    TempDirectory tmp;
    fs::create_directory(tmp.path / "album");
    test::WorkingDirectory cwd(tmp.path);
    FileQueue queue;
    const auto first = queue.add("album/song.flac", ImportOrigin::SelectedFile);
    const auto duplicate = queue.add(tmp.path / "album/../album/./song.flac",
                                     ImportOrigin::SelectedFile);
    require(first.added && !duplicate.added && first.id == duplicate.id,
            "Normalized references must identify one input");
    require(queue.jobs().size() == 1 && queue.jobs()[0].source().path.is_absolute(),
            "Queue paths must be absolute");
    require(queue.jobs()[0].state() == JobState::Checking,
            "A missing input must remain visible for probing");
}

void queue_origin() {
    TempDirectory tmp;
    FileQueue queue;
    const auto folder = queue.add(tmp.path / "song.mp3", ImportOrigin::FolderScan);
    require(queue.job(folder.id).source().origin == ImportOrigin::FolderScan,
            "Folder import origin was lost");
    const auto selected = queue.add(tmp.path / "song.mp3", ImportOrigin::SelectedFile);
    require(!selected.added && selected.origin_changed && selected.id == folder.id,
            "An explicit selection must promote a previously scanned input");
    const auto scanned = queue.add(tmp.path / "song.mp3", ImportOrigin::FolderScan);
    require(!scanned.added && !scanned.origin_changed &&
            queue.job(folder.id).source().origin == ImportOrigin::SelectedFile,
            "Folder scanning must not revoke explicit MP3 eligibility");
}

void queue_distinct_sources() {
    TempDirectory tmp;
    FileQueue queue;
    const auto first = queue.add(tmp.path / "one/song.flac", ImportOrigin::SelectedFile);
    const auto second = queue.add(tmp.path / "two/song.flac", ImportOrigin::FolderScan);
    const auto unicode = queue.add(tmp.path / fs::path(u8"音楽/Étude 🎵.flac"),
                                   ImportOrigin::SelectedFile);
    require(first.added && second.added && unicode.added && queue.jobs().size() == 3,
            "Different inputs must not merge by basename");
    require(queue.jobs()[0].id() == first.id && queue.jobs()[1].id() == second.id,
            "Queue insertion order changed");
    require(queue.job(unicode.id).source().path.filename() == fs::path(u8"Étude 🎵.flac"),
            "Unicode input path was changed");
}

void queue_alias() {
#ifndef _WIN32
    TempDirectory tmp;
    fs::create_directory(tmp.path / "real");
    test::write(tmp.path / "real/song.flac", "source");
    fs::create_directory_symlink(tmp.path / "real", tmp.path / "alias");
    FileQueue queue;
    const auto first = queue.add(tmp.path / "real/song.flac", ImportOrigin::SelectedFile);
    const auto second = queue.add(tmp.path / "alias/song.flac", ImportOrigin::SelectedFile);
    require(!second.added && first.id == second.id, "Directory aliases must deduplicate");
#endif
}

void queue_case_rules() {
    TempDirectory tmp;
    FileQueue queue;
    const auto first = queue.add(tmp.path / fs::path(u8"Äudio.flac"), ImportOrigin::SelectedFile);
    const auto second = queue.add(tmp.path / fs::path(u8"äudio.flac"), ImportOrigin::SelectedFile);
#ifdef _WIN32
    require(!second.added && second.id == first.id, "Windows Unicode case must deduplicate");
#else
    require(second.added && second.id != first.id, "Linux path case must be preserved");
#endif
}

void queue_clear() {
    TempDirectory tmp;
    FileQueue queue;
    const auto first = queue.add(tmp.path / "song.flac", ImportOrigin::SelectedFile);
    queue.clear();
    throws<std::out_of_range>([&] { (void)queue.job(first.id); }, "Cleared ID still resolves");
    const auto next = queue.add(tmp.path / "song.flac", ImportOrigin::SelectedFile);
    require(next.added && next.id != first.id, "Clearing must not reuse a stale job ID");
}

ConversionJob make_job(const fs::path& path) {
    return ConversionJob(1, {path, ImportOrigin::SelectedFile});
}

void job_success() {
    TempDirectory tmp;
    auto job = make_job(tmp.path / "song.flac");
    job.mark_ready();
    job.start();
    require(!job.progress(), "Unknown duration should have indeterminate progress");
    job.update_progress(0.25);
    require(job.progress() == 0.25, "Progress was lost");
    job.update_progress(std::nullopt);
    require(!job.progress(), "Indeterminate progress must be representable");
    job.complete(tmp.path / "song.mp3");
    require(job.state() == JobState::Completed && job.progress() == 1.0 &&
            job.output() == tmp.path / "song.mp3", "Completed job result is incomplete");
}

void job_skips() {
    TempDirectory tmp;
    auto job = make_job(tmp.path / "song.mp3");
    job.skip_same_bitrate("Verified CBR at target bitrate");
    require(job.state() == JobState::SameBitrateSkipped && !job.output() &&
            !job.detail().empty(), "Same-bitrate skip must retain its reason");
    job.reset();
    job.skip_unsupported("No usable audio stream");
    require(job.state() == JobState::UnsupportedSkipped &&
            job.detail() == "No usable audio stream", "Unsupported skip reason was lost");
}

void job_failure() {
    TempDirectory tmp;
    auto job = make_job(tmp.path / "song.flac");
    job.mark_ready();
    job.start();
    job.update_progress(0.4);
    job.fail("Encoder exited unsuccessfully");
    require(job.state() == JobState::Failed && !job.output() && job.progress() == 0.4,
            "Failed conversion must retain diagnostic state without an output");
}

void job_cancellation() {
    TempDirectory tmp;
    for (int step = 0; step < 3; ++step) {
        auto job = make_job(tmp.path / "song.flac");
        if (step > 0) job.mark_ready();
        if (step > 1) job.start();
        job.cancel();
        job.cancel();
        require(job.state() == JobState::Cancelled && !job.output(),
                "An unfinished job must be cancellable");
    }
}

void job_reset() {
    TempDirectory tmp;
    auto job = make_job(tmp.path / "song.flac");
    job.mark_ready();
    job.start();
    job.complete(tmp.path / "song.mp3");
    job.reset();
    require(job.state() == JobState::Checking && job.detail().empty() &&
            !job.progress() && !job.output(), "Rechecking must clear previous results");
    require(job.id() == 1 && job.source().path == tmp.path / "song.flac",
            "Rechecking must retain source identity");
}

void job_invalid_changes() {
    TempDirectory tmp;
    auto job = make_job(tmp.path / "song.flac");
    throws<std::logic_error>([&] { job.start(); }, "Unchecked job was started");
    throws<std::logic_error>([&] { job.complete(tmp.path / "song.mp3"); },
                             "Unchecked job was completed");
    throws<std::invalid_argument>([&] { job.skip_unsupported(""); }, "Empty reason accepted");
    job.mark_ready();
    job.start();
    throws<std::logic_error>([&] { job.reset(); }, "Running job was reset");
    for (double invalid : {-0.1, 1.1, std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()}) {
        throws<std::invalid_argument>([&] { job.update_progress(invalid); },
                                      "Invalid progress was accepted");
    }
    job.complete(tmp.path / "song.mp3");
    throws<std::logic_error>([&] { job.cancel(); }, "Completed job was cancelled");
    throws<std::logic_error>([&] { job.fail("late failure"); }, "Completed job was failed");
}

void invalid_source_paths() {
    FileQueue queue;
    for (const fs::path& path : {fs::path(), fs::path("."), fs::path(".."), fs::path("folder/")}) {
        throws<std::invalid_argument>([&] { (void)queue.add(path, ImportOrigin::SelectedFile); },
                                      "Input without a filename was accepted");
    }
    const fs::path nul(std::string("song\0.flac", 10));
    throws<std::invalid_argument>([&] { (void)queue.add(nul, ImportOrigin::SelectedFile); },
                                  "Embedded NUL was accepted");
    throws<std::invalid_argument>([&] {
        ConversionJob job(0, {"song.flac", ImportOrigin::SelectedFile});
    }, "Zero job ID was accepted");
}

} // namespace

int main(int argc, char** argv) {
    return test::run(argc, argv, {
        {"queue_normalization", queue_normalization}, {"queue_origin", queue_origin},
        {"queue_distinct_sources", queue_distinct_sources}, {"queue_alias", queue_alias},
        {"queue_case_rules", queue_case_rules}, {"queue_clear", queue_clear},
        {"job_success", job_success}, {"job_skips", job_skips},
        {"job_failure", job_failure}, {"job_cancellation", job_cancellation},
        {"job_reset", job_reset}, {"job_invalid_changes", job_invalid_changes},
        {"invalid_source_paths", invalid_source_paths}
    });
}
