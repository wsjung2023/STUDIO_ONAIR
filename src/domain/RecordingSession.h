#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/Segment.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace creator::domain {

/// Idle -> Recording -> Stopped. There is no path back to Recording: a resumed
/// take is a new session, so the segment index of a finished session can never
/// grow after the fact.
enum class SessionState {
    Idle,
    Recording,
    Stopped,
};

/// The lifecycle of one recording session and the index of segments it produced.
///
/// Pure domain state. It writes no files, talks to no encoder and knows nothing
/// about FFmpeg or the project database - the application service is what
/// persists this (CLAUDE.md 6). That is what makes the state machine testable
/// without a capture device.
///
/// Every transition returns Result rather than asserting: a double start is a
/// real thing a user can trigger by hammering the record shortcut, not a
/// programming error.
class RecordingSession final {
public:
    explicit RecordingSession(SessionId id);

    [[nodiscard]] const SessionId& id() const noexcept { return id_; }
    [[nodiscard]] SessionState state() const noexcept { return state_; }
    [[nodiscard]] std::optional<core::TimestampNs> startedAt() const noexcept;
    [[nodiscard]] std::optional<core::TimestampNs> stoppedAt() const noexcept;

    /// Fails with InvalidState unless Idle.
    [[nodiscard]] core::Result<void> start(core::TimestampNs at);

    /// Fails with InvalidState unless Recording, or InvalidArgument if `at`
    /// precedes the start time.
    [[nodiscard]] core::Result<void> stop(core::TimestampNs at);

    /// Fails with InvalidState unless Recording.
    [[nodiscard]] core::Result<void> addSegment(SegmentInfo segment);

    [[nodiscard]] const std::vector<SegmentInfo>& segments() const noexcept { return segments_; }
    [[nodiscard]] std::size_t segmentCount() const noexcept { return segments_.size(); }

    /// Wall duration of the take, or nullopt until the session has stopped.
    ///
    /// Optional rather than a zero sentinel, because zero would mean three
    /// different things at once: never started, recording right now, and a take
    /// that genuinely measured zero. A caller reading a bare zero would see
    /// "nothing is happening" while a recording is live - exactly the hidden
    /// recording state CLAUDE.md 9 forbids. nullopt makes that unreadable
    /// rather than merely documented.
    ///
    /// Not the sum of segment durations: those can have gaps a paused take
    /// leaves behind.
    [[nodiscard]] std::optional<core::DurationNs> duration() const noexcept;

private:
    SessionId id_;
    SessionState state_{SessionState::Idle};
    core::TimestampNs startedAt_{};
    core::TimestampNs stoppedAt_{};
    std::vector<SegmentInfo> segments_;
};

}  // namespace creator::domain
