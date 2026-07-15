#include "domain/RecordingSession.h"

#include <utility>

namespace creator::domain {

using core::AppError;
using core::DurationNs;
using core::ErrorCode;
using core::Result;

RecordingSession::RecordingSession(SessionId id) : id_(std::move(id)) {}

Result<void> RecordingSession::start(core::TimestampNs at) {
    if (state_ != SessionState::Idle) {
        return AppError{ErrorCode::InvalidState, "recording session has already been started"};
    }
    startedAt_ = at;
    state_ = SessionState::Recording;
    return core::ok();
}

Result<void> RecordingSession::stop(core::TimestampNs at) {
    if (state_ != SessionState::Recording) {
        return AppError{ErrorCode::InvalidState, "recording session is not recording"};
    }
    if (at < startedAt_) {
        return AppError{ErrorCode::InvalidArgument, "stop time precedes start time"};
    }
    stoppedAt_ = at;
    state_ = SessionState::Stopped;
    return core::ok();
}

Result<void> RecordingSession::addSegment(SegmentInfo segment) {
    if (state_ != SessionState::Recording) {
        return AppError{ErrorCode::InvalidState,
                        "segments may only be added while the session is recording"};
    }
    segments_.push_back(std::move(segment));
    return core::ok();
}

std::optional<DurationNs> RecordingSession::duration() const noexcept {
    if (state_ != SessionState::Stopped) {
        return std::nullopt;
    }
    return stoppedAt_ - startedAt_;
}

}  // namespace creator::domain
