#include "capture/AndroidDeviceSession.h"

#include <limits>

namespace creator::capture {

std::uint64_t AndroidDeviceSession::begin() noexcept {
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) generation_ = 0;
    ++generation_;
    state_ = DeviceSessionState::Starting;
    return generation_;
}

bool AndroidDeviceSession::markStreaming(std::uint64_t generation) noexcept {
    if (generation != generation_ || state_ != DeviceSessionState::Starting) return false;
    state_ = DeviceSessionState::Streaming;
    return true;
}

bool AndroidDeviceSession::acceptsCallbacks(std::uint64_t generation) const noexcept {
    return generation == generation_ &&
           (state_ == DeviceSessionState::Starting ||
            state_ == DeviceSessionState::Streaming);
}

bool AndroidDeviceSession::fail(std::uint64_t generation) noexcept {
    if (!acceptsCallbacks(generation)) return false;
    state_ = DeviceSessionState::Failed;
    return true;
}

bool AndroidDeviceSession::requestStop(std::uint64_t generation) noexcept {
    if (generation != generation_ ||
        (state_ != DeviceSessionState::Starting &&
         state_ != DeviceSessionState::Streaming &&
         state_ != DeviceSessionState::Failed)) {
        return false;
    }
    state_ = DeviceSessionState::Stopping;
    return true;
}

bool AndroidDeviceSession::markStopped(std::uint64_t generation) noexcept {
    if (generation != generation_ || state_ != DeviceSessionState::Stopping) return false;
    state_ = DeviceSessionState::Stopped;
    return true;
}

}  // namespace creator::capture
