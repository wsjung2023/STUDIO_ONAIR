#pragma once

#include "avatar/AvatarMotionNdjsonSink.h"
#include "avatar/AvatarProviderId.h"
#include "avatar/ExpressionNormalizer.h"
#include "avatar/ExpressionSmoother.h"
#include "avatar/ITrackingProvider.h"
#include "avatar/TrackingResult.h"
#include "core/Result.h"

#include <span>

namespace creator::avatar {

/// Converts one provider poll into one normalized, smoothed avatar.motion
/// sample and appends it to telemetry. Face selection, confidence gating,
/// calibration, and jitter filtering live here so every real provider follows
/// the same deterministic policy before data reaches the package.
class AvatarMotionPipeline final {
public:
    AvatarMotionPipeline(AvatarProviderId provider,
                         CalibrationProfile calibration,
                         AvatarMotionNdjsonSink& sink,
                         ExpressionSmoother::Constants smootherConstants = {});

    AvatarMotionPipeline(const AvatarMotionPipeline&) = delete;
    AvatarMotionPipeline& operator=(const AvatarMotionPipeline&) = delete;
    AvatarMotionPipeline(AvatarMotionPipeline&&) = delete;
    AvatarMotionPipeline& operator=(AvatarMotionPipeline&&) = delete;

    /// Processes one non-empty datagram/result batch. A batch containing no
    /// found face emits a neutral sample at that batch's timestamp, while an
    /// empty poll is rejected so a missing UDP packet cannot fabricate time.
    [[nodiscard]] core::Result<AvatarMotionSample> process(
        std::span<const TrackingResult> candidates);

    /// Processes one in-process provider frame through the same policy as an
    /// asynchronous source. The provider owns pixel interpretation; this
    /// boundary owns timestamp-preserving normalization and persistence.
    [[nodiscard]] core::Result<AvatarMotionSample> processFrame(
        ITrackingProvider& provider, const media::VideoFrame& frame);

    /// Starts a fresh tracking session without allowing the prior expression
    /// to bleed into the first sample of the next recording.
    void reset() noexcept;

private:
    AvatarProviderId provider_;
    ExpressionNormalizer normalizer_;
    ExpressionSmoother smoother_;
    AvatarMotionNdjsonSink* sink_;
};

}  // namespace creator::avatar
