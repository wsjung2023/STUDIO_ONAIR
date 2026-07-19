#include "avatar/CalibrationProfile.h"

#include <algorithm>
#include <cmath>

namespace creator::avatar {

namespace {

constexpr float kUnitLo = 0.0F;
constexpr float kUnitHi = 1.0F;
constexpr float kAngleLo = -1.0F;
constexpr float kAngleHi = 1.0F;

// Offset-removal calibration for one field: subtract the captured baseline
// and clamp to the field's documented range. See CalibrationProfile.h for
// why this replaces a gain/rescale step in Stage A, and why clamping a
// below-baseline [0,1] field to 0 is intended rather than a collapse bug.
float calibrateField(float raw, float baseline, float lo, float hi) {
    if (!std::isfinite(raw)) {
        // A non-finite raw reading means the provider failed to report
        // tracking loss the documented way (faceFound/confidence); treat the
        // field as neutral rather than let NaN/Inf leak into the output.
        return 0.0F;
    }
    return std::clamp(raw - baseline, lo, hi);
}

bool allFieldsFinite(const ExpressionParameters& p) {
    return std::isfinite(p.eyeOpenLeft) && std::isfinite(p.eyeOpenRight) &&
           std::isfinite(p.browUpLeft) && std::isfinite(p.browUpRight) &&
           std::isfinite(p.mouthOpen) && std::isfinite(p.mouthWide) &&
           std::isfinite(p.headYaw) && std::isfinite(p.headPitch) &&
           std::isfinite(p.headRoll);
}

bool fieldInRange(float value, float lo, float hi) {
    return value >= lo && value <= hi;
}

bool allFieldsInRange(const ExpressionParameters& p) {
    return fieldInRange(p.eyeOpenLeft, kUnitLo, kUnitHi) &&
           fieldInRange(p.eyeOpenRight, kUnitLo, kUnitHi) &&
           fieldInRange(p.browUpLeft, kUnitLo, kUnitHi) &&
           fieldInRange(p.browUpRight, kUnitLo, kUnitHi) &&
           fieldInRange(p.mouthOpen, kUnitLo, kUnitHi) &&
           fieldInRange(p.mouthWide, kUnitLo, kUnitHi) &&
           fieldInRange(p.headYaw, kAngleLo, kAngleHi) &&
           fieldInRange(p.headPitch, kAngleLo, kAngleHi) &&
           fieldInRange(p.headRoll, kAngleLo, kAngleHi);
}

}  // namespace

CalibrationProfile::CalibrationProfile(ExpressionParameters baseline) : baseline_(baseline) {}

CalibrationProfile CalibrationProfile::identity() {
    return CalibrationProfile(ExpressionParameters::neutral());
}

core::Result<CalibrationProfile> CalibrationProfile::fromNeutral(
    const ExpressionParameters& neutralRaw) {
    if (!allFieldsFinite(neutralRaw)) {
        return core::AppError(
            core::ErrorCode::InvalidArgument,
            "calibration neutral pose has a NaN or infinite field; capture cannot be trusted "
            "as a baseline");
    }
    if (!allFieldsInRange(neutralRaw)) {
        return core::AppError(
            core::ErrorCode::InvalidArgument,
            "calibration neutral pose has a field outside its documented range; capture cannot "
            "be trusted as a baseline");
    }
    return CalibrationProfile(neutralRaw);
}

ExpressionParameters CalibrationProfile::apply(const ExpressionParameters& raw) const {
    ExpressionParameters out{};
    out.eyeOpenLeft = calibrateField(raw.eyeOpenLeft, baseline_.eyeOpenLeft, kUnitLo, kUnitHi);
    out.eyeOpenRight = calibrateField(raw.eyeOpenRight, baseline_.eyeOpenRight, kUnitLo, kUnitHi);
    out.browUpLeft = calibrateField(raw.browUpLeft, baseline_.browUpLeft, kUnitLo, kUnitHi);
    out.browUpRight = calibrateField(raw.browUpRight, baseline_.browUpRight, kUnitLo, kUnitHi);
    out.mouthOpen = calibrateField(raw.mouthOpen, baseline_.mouthOpen, kUnitLo, kUnitHi);
    out.mouthWide = calibrateField(raw.mouthWide, baseline_.mouthWide, kUnitLo, kUnitHi);
    out.headYaw = calibrateField(raw.headYaw, baseline_.headYaw, kAngleLo, kAngleHi);
    out.headPitch = calibrateField(raw.headPitch, baseline_.headPitch, kAngleLo, kAngleHi);
    out.headRoll = calibrateField(raw.headRoll, baseline_.headRoll, kAngleLo, kAngleHi);
    return out;
}

}  // namespace creator::avatar
