#pragma once

#include "avatar/CalibrationProfile.h"
#include "avatar/ExpressionNormalizer.h"
#include "avatar/ExpressionParameters.h"
#include "avatar/TrackingResult.h"
#include "core/Result.h"

#include <cstddef>
#include <vector>

namespace creator::avatar {

/// Accumulates TrackingResult frames captured while a performer holds a
/// neutral face during a "hold neutral" calibration window, and derives a
/// robust CalibrationProfile from them.
///
/// CalibrationProfile::fromNeutral takes a single raw sample, which is
/// fragile: one jittery or momentarily-bad frame captured during the "hold
/// neutral" moment would poison the whole calibration. CalibrationCapture
/// instead collects many frames and reduces each ExpressionParameters field
/// independently to its median across the accepted frames before handing
/// that per-field baseline to fromNeutral - a single outlier frame cannot
/// drag a median the way it would drag a mean, because the median only
/// looks at the middle-ranked sample(s), not the sum of all of them.
class CalibrationCapture final {
public:
    /// Minimum number of accepted frames build() requires before it will
    /// derive a baseline. Below this, "median" stops being a meaningful
    /// robustness guarantee - e.g. with 1-2 accepted frames the "median" is
    /// just picking one of them outright, so a single bad frame could still
    /// poison the baseline the same way fromNeutral's single-sample input
    /// does today. 10 is a conservative floor sized for a few-seconds "hold
    /// neutral" capture window at typical (>=a few fps) tracking frame
    /// rates; callers may collect more before calling build().
    static constexpr std::size_t kMinCalibrationFrames = 10;

    /// A frame reporting faceFound == true but confidence below this floor is
    /// rejected by add(), for the same reason ExpressionNormalizer gates on
    /// it: too unreliable to trust as a real neutral-pose sample. Reuses
    /// ExpressionNormalizer::kMinConfidence rather than defining a second,
    /// independently-tunable threshold, so "trustworthy frame" means the
    /// same thing throughout the avatar pipeline instead of drifting apart
    /// under separate tuning.
    static constexpr float kMinConfidence = ExpressionNormalizer::kMinConfidence;

    /// Records one frame. Rejected (not counted toward acceptedCount) when:
    /// - result.faceFound is false (no face to calibrate from), or
    /// - result.confidence is below kMinConfidence (too unreliable), or
    /// - any field of result.raw is NaN or infinite (cannot contribute to a
    ///   numeric median).
    /// Accepted frames are otherwise kept as-is, unvalidated against the
    /// documented per-field range: fromNeutral() re-validates the computed
    /// median baseline against that range in build(), which is the single
    /// place that check needs to live.
    void add(const TrackingResult& result);

    /// Number of frames accepted by add() since construction or the last
    /// reset().
    [[nodiscard]] std::size_t acceptedCount() const;

    /// Number of frames rejected by add() since construction or the last
    /// reset(). Exposed so a caller can surface "N frames dropped, please
    /// hold still" feedback rather than silently discarding bad frames
    /// (CLAUDE.md 9: don't hide capture problems from the user).
    [[nodiscard]] std::size_t rejectedCount() const;

    /// Derives a CalibrationProfile from the accepted frames' per-field
    /// median, then passes that baseline through
    /// CalibrationProfile::fromNeutral (which re-validates it is finite and
    /// in-range).
    ///
    /// Fails with AppError{InvalidArgument} when fewer than
    /// kMinCalibrationFrames frames have been accepted. Also propagates
    /// fromNeutral's own AppError{InvalidArgument} if the computed median
    /// baseline is somehow out of range - this should not happen when every
    /// accepted raw frame was already in-range, but is not assumed.
    [[nodiscard]] core::Result<CalibrationProfile> build() const;

    /// Discards all accumulated frames and counters so a fresh capture can
    /// start (e.g. the user wants to redo a bad "hold neutral" take).
    void reset();

private:
    std::vector<ExpressionParameters> accepted_;
    std::size_t rejectedCount_{0};
};

}  // namespace creator::avatar
