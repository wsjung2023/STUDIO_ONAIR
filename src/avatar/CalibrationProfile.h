#pragma once

#include "avatar/ExpressionParameters.h"
#include "core/Result.h"

namespace creator::avatar {

/// Per-performer neutral-face baseline, captured once and applied to every
/// subsequent raw tracking frame so a person's own resting face reads as
/// documented-neutral (all fields 0) rather than whatever a tracking engine's
/// raw output happens to be at rest (e.g. a resting mouth that never fully
/// closes, or a camera mounted slightly off-axis so headYaw never reads 0).
///
/// apply() is a per-field, two-segment piecewise-linear remap: the captured
/// baseline for a field maps to that field's documented zero, and the
/// field's documented range endpoints ([0,1] for eye/brow/mouth, [-1,1] for
/// head angles) still map to themselves. This is why "rescale" is a real
/// step and not just an offset subtraction: a performer whose resting
/// mouthOpen reads 0.15 should still be able to reach mouthOpen == 1.0 at
/// their own fully-open mouth, not cap out at 0.85.
class CalibrationProfile final {
public:
    /// No-op passthrough profile: baseline is the documented zero for every
    /// field, so apply() returns any in-range raw value unchanged (only
    /// out-of-range input is affected, by the trailing clamp). Used until a
    /// real per-performer calibration has been captured.
    [[nodiscard]] static CalibrationProfile identity();

    /// Captures neutralRaw as the per-field baseline for later apply() calls.
    ///
    /// Fails with AppError{InvalidArgument} if any field of neutralRaw is NaN
    /// or infinite: a non-finite baseline would silently poison every later
    /// apply() call (a broken denominator in the rescale step), which
    /// CLAUDE.md 9 forbids hiding behind a plausible-looking result.
    [[nodiscard]] static core::Result<CalibrationProfile> fromNeutral(
        const ExpressionParameters& neutralRaw);

    /// Removes this profile's baseline from raw, rescales so the field's
    /// documented range endpoints are still reachable, and clamps the result
    /// to the field's documented range.
    [[nodiscard]] ExpressionParameters apply(const ExpressionParameters& raw) const;

private:
    explicit CalibrationProfile(ExpressionParameters baseline);

    ExpressionParameters baseline_;
};

}  // namespace creator::avatar
