#pragma once

#include "core/output_naming.hpp"

#include <memory>

namespace audio_converter {

// Owns a temporary MP3 and publishes it without replacing an existing entry.
// The planner must outlive the transaction. Finish all writers and media
// validation before commit; synchronize access to each transaction externally.
class OutputTransaction {
public:
    OutputTransaction(OutputPathPlanner& planner, OutputRequest request);
    ~OutputTransaction();

    OutputTransaction(const OutputTransaction&) = delete;
    OutputTransaction& operator=(const OutputTransaction&) = delete;
    OutputTransaction(OutputTransaction&&) = delete;
    OutputTransaction& operator=(OutputTransaction&&) = delete;

    [[nodiscard]] const fs::path& temporary_path() const noexcept;
    [[nodiscard]] const fs::path& final_path() const noexcept;
    [[nodiscard]] bool committed() const noexcept;
    [[nodiscard]] bool has_temporary_file() const noexcept;

    // Requires a nonempty regular file. Retries name collisions without overwrite.
    [[nodiscard]] fs::path commit();

    // Removes only the owned temporary file; completed outputs are retained.
    void discard();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace audio_converter
