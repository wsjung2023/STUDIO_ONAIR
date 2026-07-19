#pragma once

#include "core/Result.h"
#include "transcription/Transcript.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace creator::transcription {

/// Durable reader/writer for transcript artifacts inside a project package.
///
/// Responsibility: persist a Transcript as a schema-versioned JSON file in the
/// package (NOT the project DB — a transcript is a regenerable/derived artifact,
/// CLAUDE.md 6), and read it back. Writes are durable: content goes to a
/// temporary sibling file, is flushed, and is atomically renamed over the
/// target, so a crash mid-write never leaves a half-written or truncated
/// artifact (CLAUDE.md 4). Reads re-validate through the serializer, so a
/// corrupt file surfaces as a core::AppError.
///
/// RAII: holds only the directory path; owns no OS handle across calls.
class TranscriptStore final {
public:
    /// `directory` is the package folder that transcript files live in (e.g.
    /// <project>/transcripts). It is created on first write if missing.
    explicit TranscriptStore(std::filesystem::path directory)
        : directory_(std::move(directory)) {}

    /// Serializes and durably writes `transcript` to `<directory>/<name>.json`.
    /// Returns the written path on success. On any failure (directory not
    /// creatable, temp not writable, rename failed) returns an IoFailure and
    /// leaves no partial file behind. Fails with InvalidArgument if `name` is
    /// empty or contains a path separator.
    [[nodiscard]] core::Result<std::filesystem::path> write(
        std::string_view name, const Transcript& transcript) const;

    /// Reads and validates the transcript file at `path`. Returns NotFound if it
    /// does not exist, IoFailure if it cannot be read, and ParseFailure if its
    /// contents are not a valid transcript document.
    [[nodiscard]] core::Result<Transcript> read(
        const std::filesystem::path& path) const;

private:
    std::filesystem::path directory_;
};

}  // namespace creator::transcription
