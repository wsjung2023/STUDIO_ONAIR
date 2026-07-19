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
/// Stage-A model: offset-removal calibration. apply() subtracts the captured
/// baseline from each field and clamps to that field's documented range -
/// nothing more. For a `[0,1]` field (eye/brow/mouth), this means any raw
/// reading below the performer's own captured neutral clamps to 0: these
/// fields are documented as unidirectional expression magnitude (0 =
/// neutral, 1 = max), so "less open than my resting face" and "exactly my
/// resting face" both mean "no expression", and both correctly read 0. This
/// is intentional, not a bug - see CalibrationProfileTest for a dedicated
/// case. For a `[-1,1]` head-angle field, a below-baseline raw stays graded
/// (a distinct negative value per distinct input) because the field is
/// bidirectional and 0 sits in the interior of the range, not at an
/// endpoint.
///
/// A gain/rescale step (stretching the remaining travel above baseline back
/// onto the full [0,1]/[-1,1] range, so e.g. a performer whose resting
/// mouthOpen reads 0.15 could still reach mouthOpen == 1.0 without opening
/// any wider than an uncalibrated performer would need to) is a deliberate
/// Stage-B deferral: it requires knowing a real provider's (MediaPipe /
/// OpenSeeFace) actual achievable dynamic range per field, which Stage A's
/// fake provider - emitting already-normalized values - cannot validate.
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
    /// or infinite, or is finite but outside that field's documented range
    /// ([0,1] for eye/brow/mouth, [-1,1] for head angles): either way the
    /// captured pose cannot be trusted as a "this performer's neutral face"
    /// baseline, which CLAUDE.md 9 forbids hiding behind a plausible-looking
    /// result.
    [[nodiscard]] static core::Result<CalibrationProfile> fromNeutral(
        const ExpressionParameters& neutralRaw);

    /// Removes this profile's baseline from raw and clamps the result to the
    /// field's documented range. A non-finite (NaN/infinite) raw field is
    /// treated as that field's neutral() value rather than propagated: a
    /// tracking provider must report tracking loss via TrackingResult's
    /// faceFound/confidence, never by emitting NaN/Inf into a parameter
    /// field, so this is a safe, defined degradation rather than a silent
    /// pass-through of bad data.
    [[nodiscard]] ExpressionParameters apply(const ExpressionParameters& raw) const;

private:
    explicit CalibrationProfile(ExpressionParameters baseline);

    ExpressionParameters baseline_;
};

}  // namespace creator::avatar
