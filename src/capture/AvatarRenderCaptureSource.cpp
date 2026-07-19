#include "capture/AvatarRenderCaptureSource.h"

#include "core/AppError.h"

#include <utility>

namespace creator::capture {

AvatarRenderCaptureSource::AvatarRenderCaptureSource(
    domain::SourceId id, std::string displayName, Render render)
    : id_(std::move(id)), displayName_(std::move(displayName)),
      render_(std::move(render)) {}

domain::SourceId AvatarRenderCaptureSource::id() const { return id_; }

std::string AvatarRenderCaptureSource::displayName() const {
    return displayName_;
}

core::Result<void> AvatarRenderCaptureSource::start(const CaptureConfig& config) {
    if (frameRate_.has_value()) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "avatar render source is already started"};
    }
    if (!render_) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "avatar render callback is unavailable"};
    }
    auto frameRate = core::FrameRate::create(
        static_cast<std::int64_t>(config.frameRateNumerator),
        static_cast<std::int64_t>(config.frameRateDenominator));
    if (!frameRate.hasValue()) return frameRate.error();
    config_ = config;
    frameRate_ = frameRate.value();
    nextFrameIndex_ = 0;
    stats_ = {};
    stats_.currentFps = static_cast<double>(frameRate_->numerator()) /
                        static_cast<double>(frameRate_->denominator());
    return core::ok();
}

core::Result<void> AvatarRenderCaptureSource::stop() {
    frameRate_.reset();
    nextFrameIndex_ = 0;
    return core::ok();
}

CaptureStats AvatarRenderCaptureSource::stats() const noexcept { return stats_; }

core::Result<media::VideoFrame> AvatarRenderCaptureSource::tick() {
    if (!frameRate_.has_value()) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "avatar render source is not started"};
    }
    const auto timestamp = core::frameToTimestamp(nextFrameIndex_, *frameRate_);
    auto rendered = render_(timestamp);
    if (!rendered.hasValue()) return rendered.error();
    if (rendered.value().timestamp != timestamp) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "avatar renderer returned a mismatched timestamp"};
    }
    ++nextFrameIndex_;
    ++stats_.receivedFrames;
    return rendered;
}

}  // namespace creator::capture
