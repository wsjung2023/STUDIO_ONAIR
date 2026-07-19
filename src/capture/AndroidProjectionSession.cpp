#include "capture/AndroidProjectionSession.h"

#include <limits>

namespace creator::capture {

std::uint64_t AndroidProjectionSession::beginApprovedProjection() noexcept {
    // A zero generation is never handed to a Java callback. Wrap is treated as
    // a fresh non-zero epoch; reaching it requires more than 2^64 sessions.
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) generation_ = 0;
    ++generation_;
    state_ = ProjectionSessionState::Starting;
    return generation_;
}

bool AndroidProjectionSession::markStreaming(std::uint64_t generation) noexcept {
    if (generation != generation_ || state_ != ProjectionSessionState::Starting) return false;
    state_ = ProjectionSessionState::Streaming;
    return true;
}

bool AndroidProjectionSession::onProjectionRevoked(std::uint64_t generation) noexcept {
    if (generation != generation_ ||
        (state_ != ProjectionSessionState::Starting && state_ != ProjectionSessionState::Streaming)) {
        return false;
    }
    state_ = ProjectionSessionState::Revoked;
    return true;
}

bool AndroidProjectionSession::markStopped(std::uint64_t generation) noexcept {
    if (generation != generation_ ||
        (state_ != ProjectionSessionState::Streaming && state_ != ProjectionSessionState::Revoked)) {
        return false;
    }
    state_ = ProjectionSessionState::Stopped;
    return true;
}

}  // namespace creator::capture
