#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "domain/RecordingSession.h"
#include "domain/Segment.h"
#include "media/MediaTypes.h"
#include "recorder/IRecorder.h"

#include <optional>
#include <vector>

namespace creator::fakes {

/// An IRecorder that keeps the segment index in memory and writes no files.
///
/// Closes a segment every RecorderConfig::segmentDuration of frame timestamps -
/// not of wall time. Nothing here reads a clock, so a take is exactly as long
/// as the frames say it is and the tests are deterministic.
///
/// stop() flushes the trailing partial segment. Dropping it would silently lose
/// the tail of every take, and "녹화 실패를 숨기지 않는다" (CLAUDE.md 9) starts
/// with not losing footage quietly.
class FakeRecorder final : public creator::recorder::IRecorder {
public:
    [[nodiscard]] creator::core::Result<void> start(
        const creator::recorder::RecorderConfig& config,
        creator::core::TimestampNs at) override;

    [[nodiscard]] creator::core::Result<void> accept(
        const creator::media::VideoFrame& frame) override;

    [[nodiscard]] creator::core::Result<void> accept(
        const creator::media::AudioBlock& block) override;

    [[nodiscard]] creator::core::Result<creator::domain::RecordingSession> stop(
        creator::core::TimestampNs at) override;

    [[nodiscard]] creator::recorder::RecorderStats stats() const noexcept override;

    /// Segments closed so far in the current take.
    [[nodiscard]] const std::vector<creator::domain::SegmentInfo>& segments() const noexcept {
        return segments_;
    }

private:
    void closeSegment(creator::core::TimestampNs endTime);

    std::optional<creator::recorder::RecorderConfig> config_;
    std::optional<creator::domain::RecordingSession> session_;
    std::vector<creator::domain::SegmentInfo> segments_;
    creator::recorder::RecorderStats stats_{};
    creator::core::TimestampNs segmentStart_{};
    /// Timestamp of the last VIDEO frame accepted, used as the trailing
    /// segment's end time when stop() flushes it. Named for video
    /// specifically (not lastFrameTime_) because audio must never write it:
    /// segments here are video-only (see accept(AudioBlock)'s own comment),
    /// so an audio timestamp - which can legitimately arrive earlier than the
    /// current video position, since the two are independent streams - would
    /// make the trailing segment's duration wrong, including negative.
    creator::core::TimestampNs lastVideoTime_{};
    bool sawFrameInSegment_{false};
};

}  // namespace creator::fakes
