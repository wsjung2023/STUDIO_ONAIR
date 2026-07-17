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
/// directly.
class ExpressionNormalizer final {
public:
    explicit ExpressionNormalizer(CalibrationProfile profile);

    /// Returns ExpressionParameters::neutral() when result.faceFound is
    /// false: a frame where no face was found carries no meaningful
    /// expression signal, so passing raw or calibrated values through here
    /// would let a stale expression (from a previous frame, or tracker
    /// noise) leak into telemetry as if it were a real reading.
    [[nodiscard]] ExpressionParameters normalize(const TrackingResult& result) const;

private:
    CalibrationProfile profile_;
};

}  // namespace creator::avatar
