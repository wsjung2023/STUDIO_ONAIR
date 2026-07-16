#include "fakes/FakeCaptureSource.h"

#include <utility>

namespace creator::fakes {

using core::AppError;
using core::ErrorCode;
using core::Result;

FakeCaptureSource::FakeCaptureSource(domain::SourceId id, std::string displayName)
    : id_(std::move(id)), displayName_(std::move(displayName)) {}

domain::SourceId FakeCaptureSource::id() const { return id_; }

std::string FakeCaptureSource::displayName() const { return displayName_; }

Result<void> FakeCaptureSource::start(const capture::CaptureConfig& config) {
    if (failNextStart_.has_value()) {
        AppError error = std::move(*failNextStart_);
        failNextStart_.reset();
        return error;
    }
    if (started_) {
        return AppError{ErrorCode::InvalidState, "capture source is already started"};
    }

    auto rate = core::FrameRate::create(static_cast<std::int64_t>(config.frameRateNumerator),
                                        static_cast<std::int64_t>(config.frameRateDenominator));
    if (!rate.hasValue()) {
        return rate.error();
    }

    config_ = config;
    frameRate_ = rate.value();
    nextFrameIndex_ = 0;
    started_ = true;
    return core::ok();
}

Result<void> FakeCaptureSource::stop() {
    if (failNextStop_.has_value()) {
        AppError error = std::move(*failNextStop_);
        failNextStop_.reset();
        return error;
    }
    // Not an error when never started: a real source whose start() failed still
    // has to be cleaned up, so stop() has to tolerate it.
    started_ = false;
    return core::ok();
}

capture::CaptureStats FakeCaptureSource::stats() const noexcept {
    capture::CaptureStats stats;
    stats.receivedFrames = static_cast<std::uint64_t>(nextFrameIndex_);
    stats.droppedFrames = 0;
    stats.currentFps =
        frameRate_.has_value() ? static_cast<double>(frameRate_->numerator()) /
                                     static_cast<double>(frameRate_->denominator())
                               : 0.0;
    return stats;
}

Result<media::VideoFrame> FakeCaptureSource::tick() {
    if (!started_ || !frameRate_.has_value()) {
        return AppError{ErrorCode::InvalidState, "capture source is not started"};
    }

    media::VideoFrame frame;
    frame.timestamp = core::frameToTimestamp(nextFrameIndex_, *frameRate_);
    frame.width = config_.targetWidth;
    frame.height = config_.targetHeight;
    frame.pixelFormat = media::PixelFormat::Bgra8;
    frame.colorSpace = media::ColorSpace::Rec709Sdr;
    frame.platformHandle = nullptr;  // no pixels: this fake proves timing only

    ++nextFrameIndex_;
    return frame;
}

void FakeCaptureSource::failNextStart(AppError error) { failNextStart_ = std::move(error); }

void FakeCaptureSource::failNextStop(AppError error) { failNextStop_ = std::move(error); }

}  // namespace creator::fakes
