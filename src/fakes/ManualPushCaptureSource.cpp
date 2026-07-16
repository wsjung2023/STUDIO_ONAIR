#include "fakes/ManualPushCaptureSource.h"

#include "core/AppError.h"

#include <utility>

namespace creator::fakes {

using core::AppError;
using core::ErrorCode;
using core::Result;

ManualPushCaptureSource::ManualPushCaptureSource(
    domain::SourceId id, std::string displayName,
    std::shared_ptr<capture::IVideoFrameSink> sink)
    : id_(std::move(id)), displayName_(std::move(displayName)), sink_(std::move(sink)) {}

domain::SourceId ManualPushCaptureSource::id() const { return id_; }

std::string ManualPushCaptureSource::displayName() const { return displayName_; }

Result<void> ManualPushCaptureSource::start(const capture::CaptureConfig& config) {
    if (started_) {
        return AppError{ErrorCode::InvalidState, "capture source is already started"};
    }
    if (!sink_) {
        return AppError{ErrorCode::InvalidState, "capture source has no frame sink"};
    }
    if (config.targetWidth == 0 || config.targetHeight == 0 ||
        config.frameRateNumerator == 0 || config.frameRateDenominator == 0) {
        return AppError{ErrorCode::InvalidArgument, "capture configuration must be positive"};
    }
    stats_ = {};
    started_ = true;
    sink_->onCaptureStarted();
    return core::ok();
}

Result<void> ManualPushCaptureSource::stop() {
    started_ = false;
    return core::ok();
}

capture::CaptureStats ManualPushCaptureSource::stats() const noexcept { return stats_; }

Result<void> ManualPushCaptureSource::pushFrame(media::VideoFrame frame) {
    if (!started_) {
        return AppError{ErrorCode::InvalidState, "capture source is not started"};
    }
    sink_->onVideoFrame(std::move(frame));
    ++stats_.receivedFrames;
    return core::ok();
}

Result<void> ManualPushCaptureSource::fail(AppError error) {
    if (!started_) {
        return AppError{ErrorCode::InvalidState, "capture source is not started"};
    }
    started_ = false;
    sink_->onCaptureError(std::move(error));
    return core::ok();
}

}  // namespace creator::fakes
