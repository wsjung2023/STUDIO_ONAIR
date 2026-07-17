#include "fakes/FakeTrackingProvider.h"

#include <algorithm>
#include <utility>

namespace creator::fakes {

using avatar::AvatarProviderId;
using avatar::TrackingResult;
using core::AppError;
using core::ErrorCode;
using core::Result;

AvatarProviderId FakeTrackingProvider::defaultProviderId() {
    return AvatarProviderId::create("fake-tracker").value();
}

FakeTrackingProvider::FakeTrackingProvider(std::vector<ScriptedFrame> script, AvatarProviderId id)
    : script_(std::move(script)), id_(std::move(id)) {}

AvatarProviderId FakeTrackingProvider::providerId() const { return id_; }

Result<TrackingResult> FakeTrackingProvider::process(const media::VideoFrame& frame) {
    if (script_.empty()) {
        return AppError{ErrorCode::InvalidState,
                         "FakeTrackingProvider has an empty script; nothing to emit"};
    }

    // Clamp rather than cycle: see the header doc on process().
    const std::size_t index = std::min(nextIndex_, script_.size() - 1);
    const ScriptedFrame& scripted = script_[index];
    if (nextIndex_ < script_.size()) {
        ++nextIndex_;
    }

    TrackingResult result{};
    result.timestamp = frame.timestamp;  // the only field ever read from the frame
    result.raw = scripted.parameters;
    result.confidence = scripted.confidence;
    result.faceFound = scripted.faceFound;
    return result;
}

}  // namespace creator::fakes
