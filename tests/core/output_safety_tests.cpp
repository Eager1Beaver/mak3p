#include "core/output_transaction.hpp"
#include "test_support.hpp"

#include <barrier>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace audio_converter;
using test::require;
using test::TempDirectory;
using test::throws;

namespace {

void output_commit() {
    TempDirectory tmp;
    const auto source = tmp.path / "song.mp3";
    const std::string original("original\0bytes", 14);
    test::write(source, original);
    fs::path final;
    fs::path temporary;
    OutputPathPlanner planner;
    {
        OutputTransaction output(planner, {source, std::nullopt});
        temporary = output.temporary_path();
        require(temporary.filename().string().starts_with(".mak3p-") &&
                temporary.parent_path() == tmp.path && temporary.extension() == ".mp3" &&
                fs::is_regular_file(temporary) && !fs::exists(output.final_path()),
                "Temporary output must exist without exposing a final placeholder");
        test::write(temporary, "encoded bytes");
        final = output.commit();
        require(final == tmp.path / "song (1).mp3" && output.committed() &&
                !output.has_temporary_file() && !fs::exists(temporary),
                "Commit did not publish and retire the temporary output");
        require(output.commit() == final, "Repeated commit must retain the same output");
        output.discard();
    }
    require(test::read(source) == original && test::read(final) == "encoded bytes",
            "Commit or cleanup changed the source or completed output");
}

void output_late_conflict() {
    TempDirectory tmp;
    OutputPathPlanner planner;
    OutputTransaction output(planner, {tmp.path / "song.flac", tmp.path});
    test::write(output.temporary_path(), "encoded");
    const auto reserved = output.final_path();
    test::write(reserved, "external writer");
    const auto final = output.commit();
    require(final.filename() == "song (1).mp3" && test::read(reserved) == "external writer" &&
            test::read(final) == "encoded", "A late file collision must retry without overwrite");
}

void output_reserved_retry() {
    TempDirectory tmp;
    OutputPathPlanner planner;
    OutputTransaction first(planner, {tmp.path / "one/song.flac", tmp.path});
    OutputTransaction second(planner, {tmp.path / "two/song.flac", tmp.path});
    test::write(first.final_path(), "external");
    test::write(first.temporary_path(), "first");
    test::write(second.temporary_path(), "second");
    const auto first_path = first.commit();
    const auto second_path = second.commit();
    require(first_path.filename() == "song (2).mp3" &&
            second_path.filename() == "song (1).mp3" &&
            test::read(first_path) == "first" && test::read(second_path) == "second",
            "A collision retry took another queued job's reservation");
}

void output_directory_conflict() {
    TempDirectory tmp;
    OutputPathPlanner planner;
    OutputTransaction output(planner, {tmp.path / "song.flac", tmp.path});
    test::write(output.temporary_path(), "encoded");
    const auto reserved = output.final_path();
    fs::create_directory(reserved);
    const auto final = output.commit();
    require(final.filename() == "song (1).mp3" && fs::is_directory(reserved),
            "A late directory collision must be preserved");
}

void output_symlink_conflict() {
#ifndef _WIN32
    TempDirectory tmp;
    const auto target = tmp.path / "existing.mp3";
    test::write(target, "existing");
    OutputPathPlanner planner;
    OutputTransaction output(planner, {tmp.path / "song.flac", tmp.path});
    test::write(output.temporary_path(), "encoded");
    const auto reserved = output.final_path();
    fs::create_symlink(target, reserved);
    const auto final = output.commit();
    require(final.filename() == "song (1).mp3" && test::read(target) == "existing" &&
            fs::is_symlink(fs::symlink_status(reserved)),
            "A late symlink collision must preserve both the link and its target");
#endif
}

void output_cleanup() {
    TempDirectory tmp;
    const auto unrelated = tmp.path / ".unrelated.partial.mp3";
    test::write(unrelated, "unrelated");
    OutputPathPlanner planner;
    fs::path temporary;
    {
        OutputTransaction output(planner, {tmp.path / "song.flac", tmp.path});
        temporary = output.temporary_path();
        test::write(temporary, "partial");
        output.discard();
        output.discard();
        require(!fs::exists(temporary) && !output.committed() && !output.has_temporary_file(),
                "Explicit cancellation must remove its own temporary output");
    }
    {
        OutputTransaction output(planner, {tmp.path / "other.flac", tmp.path});
        temporary = output.temporary_path();
        test::write(temporary, "partial");
    }
    require(!fs::exists(temporary) && test::read(unrelated) == "unrelated",
            "Destructor cleanup must preserve unrelated temporary files");
}

void output_unwinding() {
    TempDirectory tmp;
    OutputPathPlanner planner;
    fs::path temporary;
    try {
        OutputTransaction output(planner, {tmp.path / "song.flac", tmp.path});
        temporary = output.temporary_path();
        test::write(temporary, "partial");
        throw std::runtime_error("simulated encoder failure");
    } catch (const std::runtime_error&) {
    }
    require(!temporary.empty() && !fs::exists(temporary) && fs::is_empty(tmp.path),
            "Exception unwinding must clean the failed job's temporary file");
}

void output_empty() {
    TempDirectory tmp;
    OutputPathPlanner planner;
    fs::path temporary;
    {
        OutputTransaction output(planner, {tmp.path / "song.flac", tmp.path});
        temporary = output.temporary_path();
        throws<std::runtime_error>([&] { (void)output.commit(); }, "Empty output was committed");
        require(!output.committed() && !fs::exists(output.final_path()),
                "An empty output must never appear under the final name");
    }
    require(!fs::exists(temporary), "Rejected empty output was not cleaned");
}

void output_discarded() {
    TempDirectory tmp;
    OutputPathPlanner planner;
    OutputTransaction output(planner, {tmp.path / "song.flac", tmp.path});
    output.discard();
    throws<std::logic_error>([&] { (void)output.commit(); }, "Discarded output was committed");
}

void output_unique_temporary() {
    TempDirectory tmp;
    OutputPathPlanner first_planner;
    OutputPathPlanner second_planner;
    OutputTransaction first(first_planner, {tmp.path / "song.flac", tmp.path});
    OutputTransaction second(second_planner, {tmp.path / "song.flac", tmp.path});
    require(first.temporary_path() != second.temporary_path() &&
            fs::is_regular_file(first.temporary_path()) && fs::is_regular_file(second.temporary_path()),
            "Independent jobs must own different exclusively created temporary files");
}

void output_replaced_temporary() {
    TempDirectory tmp;
    OutputPathPlanner planner;
    fs::path replacement;
    {
        OutputTransaction output(planner, {tmp.path / "song.flac", tmp.path});
        replacement = output.temporary_path();
        fs::remove(replacement);
        test::write(replacement, "belongs to another process");
        throws<std::runtime_error>([&] { (void)output.commit(); }, "Substituted file was published");
        throws<std::runtime_error>([&] { output.discard(); }, "Substituted file was deleted");
        require(!fs::exists(output.final_path()), "Substitution created a final output");
    }
    require(test::read(replacement) == "belongs to another process",
            "Destructor deleted or modified a substituted file");
}

void output_missing_temporary() {
    TempDirectory tmp;
    OutputPathPlanner planner;
    OutputTransaction output(planner, {tmp.path / "song.flac", tmp.path});
    fs::remove(output.temporary_path());
    throws<std::runtime_error>([&] { (void)output.commit(); }, "Missing file was published");
    output.discard();
    require(!output.has_temporary_file() && !output.committed(), "Missing file cleanup failed");
}

void output_symlink_temporary() {
#ifndef _WIN32
    TempDirectory tmp;
    const auto target = tmp.path / "original.mp3";
    test::write(target, "original");
    OutputPathPlanner planner;
    fs::path link;
    {
        OutputTransaction output(planner, {tmp.path / "song.flac", tmp.path});
        link = output.temporary_path();
        fs::remove(link);
        fs::create_symlink(target, link);
        throws<std::runtime_error>([&] { (void)output.commit(); }, "Symlink was published");
    }
    require(fs::is_symlink(fs::symlink_status(link)) && test::read(target) == "original",
            "Cleanup removed a substituted symlink or modified its target");
#endif
}

void output_concurrent() {
    TempDirectory tmp;
    constexpr int count = 24;
    test::write(tmp.path / "song.mp3", "existing output");
    std::vector<std::unique_ptr<OutputPathPlanner>> planners;
    for (int index = 0; index < 4; ++index) {
        planners.push_back(std::make_unique<OutputPathPlanner>());
    }
    std::vector<std::unique_ptr<OutputTransaction>> outputs;
    for (int index = 0; index < count; ++index) {
        outputs.push_back(std::make_unique<OutputTransaction>(
            *planners[static_cast<std::size_t>(index) % planners.size()],
            OutputRequest{tmp.path / "song.flac", tmp.path}));
        test::write(outputs.back()->temporary_path(), "encoded " + std::to_string(index));
    }
    std::barrier start(count);
    std::vector<fs::path> results(count);
    std::vector<std::jthread> threads;
    std::mutex errors_mutex;
    std::vector<std::string> errors;
    for (int index = 0; index < count; ++index) {
        threads.emplace_back([&, index] {
            start.arrive_and_wait();
            try {
                results[static_cast<std::size_t>(index)] =
                    outputs[static_cast<std::size_t>(index)]->commit();
            } catch (const std::exception& error) {
                std::lock_guard lock(errors_mutex);
                errors.emplace_back(error.what());
            }
        });
    }
    threads.clear();
    require(errors.empty(), errors.empty() ? "" : errors.front());
    std::set<fs::path> distinct(results.begin(), results.end());
    require(distinct.size() == count && test::read(tmp.path / "song.mp3") == "existing output",
            "Concurrent publication overwrote a file or reused a name");
    for (int index = 0; index < count; ++index) {
        require(test::read(results[static_cast<std::size_t>(index)]) ==
                    "encoded " + std::to_string(index),
                "Concurrent publication mixed output bytes");
    }
    require(static_cast<int>(std::distance(fs::directory_iterator(tmp.path),
                                         fs::directory_iterator{})) == count + 1,
            "Concurrent publication left temporary files behind");
}

void output_unwritable() {
#ifndef _WIN32
    if (::geteuid() == 0) {
        std::cout << "SKIP permission assertion: root bypasses directory permissions\n";
        return;
    }
    TempDirectory tmp;
    fs::permissions(tmp.path, fs::perms::owner_read | fs::perms::owner_exec);
    OutputPathPlanner planner;
    throws<fs::filesystem_error>([&] {
        OutputTransaction output(planner, {tmp.path / "song.flac", tmp.path});
    }, "Unwritable destination did not fail safely");
    fs::permissions(tmp.path, fs::perms::owner_all);
    require(fs::is_empty(tmp.path), "Failed creation left an output behind");
#endif
}

void output_unicode() {
    TempDirectory tmp;
    const auto folder = tmp.path / fs::path(u8"日本語 🎵");
    fs::create_directory(folder);
    OutputPathPlanner planner;
    OutputTransaction output(planner, {folder / fs::path(u8"Été.flac"), std::nullopt});
    test::write(output.temporary_path(), "encoded");
    const auto final = output.commit();
    require(final == folder / fs::path(u8"Été.mp3") && test::read(final) == "encoded",
            "Unicode destination or source basename was corrupted");
}

void output_failure_cleanup() {
#ifndef _WIN32
    if (::geteuid() == 0) {
        std::cout << "SKIP permission assertion: root bypasses directory permissions\n";
        return;
    }
    TempDirectory tmp;
    OutputPathPlanner planner;
    OutputTransaction output(planner, {tmp.path / "song.flac", tmp.path});
    const auto temporary = output.temporary_path();
    test::write(temporary, "encoded");
    fs::permissions(tmp.path, fs::perms::owner_read | fs::perms::owner_exec);
    throws<fs::filesystem_error>([&] { (void)output.commit(); },
                                 "A publication permission failure was ignored");
    fs::permissions(tmp.path, fs::perms::owner_all);
    require(!output.committed() && !fs::exists(output.final_path()) && fs::exists(temporary),
            "A failed publication must not expose the final output");
    output.discard();
    require(!fs::exists(temporary), "Failed publication could not be cleaned");
#endif
}

} // namespace

int main(int argc, char** argv) {
    return test::run(argc, argv, {
        {"output_commit", output_commit}, {"output_late_conflict", output_late_conflict},
        {"output_reserved_retry", output_reserved_retry},
        {"output_directory_conflict", output_directory_conflict},
        {"output_symlink_conflict", output_symlink_conflict},
        {"output_cleanup", output_cleanup}, {"output_unwinding", output_unwinding},
        {"output_empty", output_empty}, {"output_discarded", output_discarded},
        {"output_unique_temporary", output_unique_temporary},
        {"output_replaced_temporary", output_replaced_temporary},
        {"output_missing_temporary", output_missing_temporary},
        {"output_symlink_temporary", output_symlink_temporary},
        {"output_concurrent", output_concurrent}, {"output_unwritable", output_unwritable},
        {"output_unicode", output_unicode}, {"output_failure_cleanup", output_failure_cleanup}
    });
}
