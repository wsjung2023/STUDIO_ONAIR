#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/RecordingSession.h"
#include "media/MediaTypes.h"

#include <chrono>
#include <cstdint>

namespace creator::recorder {

/// How one source's track is written.
///
/// segmentDuration defaults to the 2 seconds ARCHITECTURE.md 6.2 specifies:
/// short enough that a crash loses little, long enough that the segment index
/// does not dominate the file count.
struct RecorderConfig final {
    creator::domain::SessionId sessionId;
    creator::domain::SourceId sourceId;
    creator::core::DurationNs segmentDuration{std::chrono::seconds{2}};
};

/// Live counters for the diagnostics surface.
///
/// framesDropped is reported, never swallowed. An encoder that cannot keep up
/// must drop and count rather than queue without bound (ARCHITECTURE.md 6.3,
/// CLAUDE.md 9 forbids 무한 queue).
struct RecorderStats final {
    std::uint64_t segmentsWritten{0};
    std::uint64_t framesAccepted{0};
    std::uint64_t framesDropped{0};
    std::uint64_t blocksAccepted{0};
};

/// Writes one source's frames to segment files and maintains the segment index.
///
/// The recorder owns the bytes; RecordingSession owns the index. Keeping them
/// apart is what lets the session state machine be tested with no encoder and
/// no disk.
///
/// Implementations must never block the UI thread (CLAUDE.md 9 forbids
/// encoding and file I/O on it).
class IRecorder {
public:
    virtual ~IRecorder() = default;

    IRecorder(const IRecorder&) = delete;
    IRecorder& operator=(const IRecorder&) = delete;
    IRecorder(IRecorder&&) = delete;
    IRecorder& operator=(IRecorder&&) = delete;

    /// Fails with InvalidState if already recording.
    [[nodiscard]] virtual creator::core::Result<void> start(const RecorderConfig& config,
                                                            creator::core::TimestampNs at) = 0;

    /// Hands a frame to the recorder. Returning ok() does not mean the frame was
    /// written - it may have been dropped under backpressure, which stats()
    /// reports. It means the recorder is still healthy.
    [[nodiscard]] virtual creator::core::Result<void> accept(
        const creator::media::VideoFrame& frame) = 0;

    /// Hands an audio block to the recorder. Same contract as the video
    /// overload: ok() means the recorder is healthy, not that the block was
    /// written.
    ///
    /// Audio is the master stream for synchronisation (ARCHITECTURE.md 5.2) and
    /// audio loss must never be silently ignored (CLAUDE.md 5), so a recorder
    /// that cannot keep up with audio reports an error here rather than
    /// dropping and counting the way it may for video.
    [[nodiscard]] virtual creator::core::Result<void> accept(
        const creator::media::AudioBlock& block) = 0;

    /// Finalises the take and returns its session, segment index included.
    /// Fails with InvalidState if not recording.
    [[nodiscard]] virtual creator::core::Result<creator::domain::RecordingSession> stop(
        creator::core::TimestampNs at) = 0;

    [[nodiscard]] virtual RecorderStats stats() const noexcept = 0;

protected:
    IRecorder() = default;
};

}  // namespace creator::recorder
