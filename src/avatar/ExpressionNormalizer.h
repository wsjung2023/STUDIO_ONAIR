#pragma once

#include "avatar/CalibrationProfile.h"
#include "avatar/ExpressionParameters.h"
#include "avatar/TrackingResult.h"

namespace creator::avatar {

/// Turns one provider's raw TrackingResult into calibrated, range-clamped
/// ExpressionParameters. This is the single place per-performer calibration
/// is applied and the "no stale expression leaks through" rule (see
/// TrackingResult's doc on faceFound) is enforced, so callers downstream
/// (the AvatarMotionSample pipeline) never touch raw provider output
/// directly. It is also the single place TrackingResult::confidence is
/// consumed: a frame reporting faceFound == true but a low confidence is
/// gated to neutral the same as a no-face frame, so a shaky reading cannot
/// leak into telemetry as if it were a trustworthy expression.
class ExpressionNormalizer final {
public:
    /// Below this confidence, a frame is treated like faceFound == false: the
    /// reading is too unreliable to trust as a real expression. 0.15 is a
    /// conservative default chosen for Stage A's fake/deterministic provider,
    /// which reports confidence as a simple scripted value rather than a
    /// calibrated engine metric; it is expected to be re-tuned in Stage B
    /// once a real tracker's (MediaPipe/OpenSeeFace) confidence distribution
    /// is known. See the constructor's minConfidence parameter to override
    /// it per instance.
    static constexpr float kMinConfidence = 0.15F;

    /// minConfidence defaults to kMinConfidence but is caller-overridable so
    /// it can be tuned per-provider once real confidence characteristics are
    /// known (Stage B), without changing this class's shape.
    explicit ExpressionNormalizer(CalibrationProfile profile, float minConfidence = kMinConfidence);

    /// Returns ExpressionParameters::neutral() when result.faceFound is
    /// false, or when result.confidence is below this instance's
    /// minConfidence threshold: either case carries no meaningful expression
    /// signal, so passing raw or calibrated values through here would let a
    /// stale or untrustworthy expression (from a previous frame, tracker
    /// noise, or a shaky low-confidence reading) leak into telemetry as if it
    /// were a real reading. Otherwise applies calibration and range-clamps
    /// the result.
    [[nodiscard]] ExpressionParameters normalize(const TrackingResult& result) const;

private:
    CalibrationProfile profile_;
    float minConfidence_;
};

}  // namespace creator::avatar
