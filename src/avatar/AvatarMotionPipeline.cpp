#include "avatar/AvatarMotionPipeline.h"

#include "avatar/PrimaryFaceSelector.h"
#include "core/AppError.h"

namespace creator::avatar {

AvatarMotionPipeline::AvatarMotionPipeline(
    AvatarProviderId provider, CalibrationProfile calibration,
    AvatarMotionNdjsonSink& sink,
    ExpressionSmoother::Constants smootherConstants)
    : provider_(std::move(provider)),
      normalizer_(std::move(calibration)),
      smoother_(smootherConstants),
      sink_(&sink) {}

core::Result<AvatarMotionSample> AvatarMotionPipeline::process(
    std::span<const TrackingResult> candidates) {
    if (candidates.empty()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "avatar motion batch must not be empty"};
    }
    const auto selected = selectPrimaryFace(candidates);
    TrackingResult neutral{};
    neutral.timestamp = candidates.front().timestamp;
    neutral.faceFound = false;
    neutral.confidence = 0.0F;
    const auto& tracking = selected.has_value() ? *selected : neutral;
    const auto smoothed = smoother_.smooth(
        normalizer_.normalize(tracking), tracking.timestamp);
    AvatarMotionSample sample{.timestamp = tracking.timestamp,
                              .parameters = smoothed,
                              .provider = provider_};
    auto appended = sink_->append(sample);
    if (!appended.hasValue()) return appended.error();
    return sample;
}

core::Result<AvatarMotionSample> AvatarMotionPipeline::processFrame(
    ITrackingProvider& provider, const media::VideoFrame& frame) {
    auto result = provider.process(frame);
    if (!result.hasValue()) return result.error();
    const auto& tracking = result.value();
    return process(std::span<const TrackingResult>{&tracking, 1});
}

void AvatarMotionPipeline::reset() noexcept { smoother_.reset(); }

}  // namespace creator::avatar
