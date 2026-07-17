#include "avatar/CalibrationProfile.h"

#include <algorithm>
#include <cmath>

namespace creator::avatar {

namespace {

constexpr float kUnitLo = 0.0F;
constexpr float kUnitHi = 1.0F;
constexpr float kAngleLo = -1.0F;
constexpr float kAngleHi = 1.0F;

// Below this denominator magnitude, the captured baseline sits at (or past)
// the field's own range endpoint: there is no travel left on that side to
// rescale from, so the honest answer is "no signal", not a divide-by-near-zero
// blow-up.
constexpr float kDegenerateDenominator = 1e-6F;

// Two-segment piecewise-linear remap for one field: baseline maps to 0 (the
// field's documented zero, matching ExpressionParameters::neutral()), and
// whichever range endpoint raw is heading toward (hi above baseline, lo
// below) still maps to itself. See CalibrationProfile.h for why this is a
// rescale and not just an offset subtraction.
float remapField(float raw, float baseline, float lo, float hi) {
    float normalized = 0.0F;
    if (raw >= baseline) {
        const float denom = hi - baseline;
        if (denom > kDegenerateDenominator) {
            normalized = (raw - baseline) / denom * hi;
        }
    } else {
        const float denom = baseline - lo;
        if (denom > kDegenerateDenominator) {
            normalized = (raw - baseline) / denom * (-lo);
        }
    }
    return std::clamp(normalized, lo, hi);
}

bool allFieldsFinite(const ExpressionParameters& p) {
    return std::isfinite(p.eyeOpenLeft) && std::isfinite(p.eyeOpenRight) &&
           std::isfinite(p.browUpLeft) && std::isfinite(p.browUpRight) &&
           std::isfinite(p.mouthOpen) && std::isfinite(p.mouthWide) &&
           std::isfinite(p.headYaw) && std::isfinite(p.headPitch) &&
           std::isfinite(p.headRoll);
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
    return CalibrationProfile(neutralRaw);
}

ExpressionParameters CalibrationProfile::apply(const ExpressionParameters& raw) const {
    ExpressionParameters out{};
    out.eyeOpenLeft = remapField(raw.eyeOpenLeft, baseline_.eyeOpenLeft, kUnitLo, kUnitHi);
    out.eyeOpenRight = remapField(raw.eyeOpenRight, baseline_.eyeOpenRight, kUnitLo, kUnitHi);
    out.browUpLeft = remapField(raw.browUpLeft, baseline_.browUpLeft, kUnitLo, kUnitHi);
    out.browUpRight = remapField(raw.browUpRight, baseline_.browUpRight, kUnitLo, kUnitHi);
    out.mouthOpen = remapField(raw.mouthOpen, baseline_.mouthOpen, kUnitLo, kUnitHi);
    out.mouthWide = remapField(raw.mouthWide, baseline_.mouthWide, kUnitLo, kUnitHi);
    out.headYaw = remapField(raw.headYaw, baseline_.headYaw, kAngleLo, kAngleHi);
    out.headPitch = remapField(raw.headPitch, baseline_.headPitch, kAngleLo, kAngleHi);
    out.headRoll = remapField(raw.headRoll, baseline_.headRoll, kAngleLo, kAngleHi);
    return out;
}

}  // namespace creator::avatar
