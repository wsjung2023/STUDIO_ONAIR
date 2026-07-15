#include "fakes/FakeRecorder.h"

#include <array>
#include <cstdio>
#include <string>
#include <utility>

namespace creator::fakes {

using core::AppError;
using core::ErrorCode;
using core::Result;
using core::TimestampNs;
using domain::RecordingSession;
using domain::SegmentInfo;
using domain::SegmentStatus;

namespace {

/// Mirrors the layout in README.md: media/<source>/segment_000001.mkv.
std::string segmentPath(const std::string& sourceId, std::uint64_t index) {
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "segment_%06llu.mkv",
                  static_cast<unsigned long long>(index));
    return "media/" + sourceId + "/" + buffer.data();
}

}  // namespace

Result<void> FakeRecorder::start(const recorder::RecorderConfig& config, TimestampNs at) {
    if (session_.has_value() && session_->state() == domain::SessionState::Recording) {
        return AppError{ErrorCode::InvalidState, "recorder is already recording"};
    }
    if (config.segmentDuration <= core::DurationNs::zero()) {
        return AppError{ErrorCode::InvalidArgument, "segment duration must be positive"};
    }

    RecordingSession session{config.sessionId};
    if (auto started = session.start(at); !started.hasValue()) {
        return started.error();
    }

    config_ = config;
    session_ = std::move(session);
    segments_.clear();
    stats_ = recorder::RecorderStats{};
    segmentStart_ = at;
    lastFrameTime_ = at;
    sawFrameInSegment_ = false;
    return core::ok();
}

Result<void> FakeRecorder::accept(const media::VideoFrame& frame) {
    if (!session_.has_value() || session_->state() != domain::SessionState::Recording) {
        return AppError{ErrorCode::InvalidState, "recorder is not recording"};
    }

    ++stats_.framesAccepted;
    lastFrameTime_ = frame.timestamp;
    sawFrameInSegment_ = true;

    // Segment boundaries follow frame timestamps, never wall time.
    while (frame.timestamp - segmentStart_ >= config_->segmentDuration) {
        closeSegment(segmentStart_ + config_->segmentDuration);
    }
    return core::ok();
}

Result<void> FakeRecorder::accept(const media::AudioBlock& block) {
    if (!session_.has_value() || session_->state() != domain::SessionState::Recording) {
        return AppError{ErrorCode::InvalidState, "recorder is not recording"};
    }

    // Counted but not segmented. Audio segmentation belongs to the real
    // recorder in R0-05, where audio and video are separate tracks with their
    // own files (README package layout: audio/microphone/segment_000001.mka).
    // Driving segment boundaries off audio here would make the fake's segment
    // count depend on which stream arrived first, and the tests assert exact
    // counts.
    ++stats_.blocksAccepted;
    lastFrameTime_ = block.timestamp;
    return core::ok();
}

Result<RecordingSession> FakeRecorder::stop(TimestampNs at) {
    if (!session_.has_value() || session_->state() != domain::SessionState::Recording) {
        return AppError{ErrorCode::InvalidState, "recorder is not recording"};
    }

    // Flush whatever is still open, or the tail of every take disappears.
    if (sawFrameInSegment_) {
        closeSegment(lastFrameTime_);
    }

    if (auto stopped = session_->stop(at); !stopped.hasValue()) {
        return stopped.error();
    }
    return *session_;
}

recorder::RecorderStats FakeRecorder::stats() const noexcept { return stats_; }

void FakeRecorder::closeSegment(TimestampNs endTime) {
    const std::uint64_t index = static_cast<std::uint64_t>(segments_.size());
    SegmentInfo segment{
        .index = index,
        .sourceId = config_->sourceId,
        .startTime = segmentStart_,
        .duration = endTime - segmentStart_,
        .status = SegmentStatus::Ready,
        .relativePath = segmentPath(config_->sourceId.value(), index),
    };

    // addSegment can only fail if the session is not recording, which accept()
    // and stop() have already checked.
    if (session_->addSegment(segment).hasValue()) {
        segments_.push_back(std::move(segment));
        ++stats_.segmentsWritten;
    }

    segmentStart_ = endTime;
    sawFrameInSegment_ = false;
}

}  // namespace creator::fakes
