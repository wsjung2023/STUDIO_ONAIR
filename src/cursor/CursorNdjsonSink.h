#pragma once

#include "core/Result.h"
#include "cursor/CursorClickEvent.h"
#include "cursor/CursorMoveEvent.h"

#include <filesystem>
#include <fstream>
#include <memory>

namespace creator::cursor {

/// Appends serialized cursor events, one per line, to a telemetry NDJSON file.
///
/// This is the durable landing spot for the cursor stream inside the project
/// package's telemetry area (e.g. telemetry/cursor/<...>.ndjson). It writes ONLY
/// to that flat file, never to the project database (CLAUDE.md §6): the database
/// row that references the finished stream is written elsewhere, by an
/// application service.
///
/// Contract:
///  - serialize-before-write: an event is serialized first, and only a
///    successfully serialized event produces bytes, so a rejected event writes
///    no partial line;
///  - ordered: events append in call order, one JSON object per '\n'-terminated
///    line;
///  - flushed per line, so a crash keeps every already-accepted event whole up
///    to the last complete newline (the design doc's recovery scan relies on
///    this);
///  - RAII: the underlying file is opened by create() and closed when the sink
///    is destroyed.
class CursorNdjsonSink final {
public:
    CursorNdjsonSink(const CursorNdjsonSink&) = delete;
    CursorNdjsonSink& operator=(const CursorNdjsonSink&) = delete;
    CursorNdjsonSink(CursorNdjsonSink&&) = delete;
    CursorNdjsonSink& operator=(CursorNdjsonSink&&) = delete;
    ~CursorNdjsonSink() = default;

    /// Opens `path` for appending. Fails with IoFailure if the file cannot be
    /// opened. Returns a heap-owned sink so the owned std::ofstream never has to
    /// move.
    [[nodiscard]] static core::Result<std::unique_ptr<CursorNdjsonSink>> create(
        const std::filesystem::path& path);

    /// Serializes and appends one cursor move event. Fails with the serializer's
    /// error (writing nothing) if serialization is rejected, or IoFailure if the
    /// write or flush fails.
    [[nodiscard]] core::Result<void> write(const CursorMoveEvent& event);

    /// Serializes and appends one cursor click event, same contract as above.
    [[nodiscard]] core::Result<void> write(const CursorClickEvent& event);

private:
    explicit CursorNdjsonSink(std::ofstream out) : out_(std::move(out)) {}

    [[nodiscard]] core::Result<void> writeLine(const std::string& line);

    std::ofstream out_;
};

}  // namespace creator::cursor
