#pragma once

#include "core/Result.h"
#include "core/Timebase.h"

#include <chrono>
#include <cmath>

namespace creator::autozoom {

/// Tunable, validated knobs for the auto-zoom heuristic.
///
/// Every field carries a unit-typed value (std::chrono for time, normalized
/// fractions for space) so a caller can never pass a bare "500" and mean the
/// wrong thing (CLAUDE.md §4). The documented defaults below are the ones
/// AutoZoomAnalyzer uses when a caller does not override them; they are tuned
/// for screen-recording cursor telemetry sampled at tens of Hz.
///
/// Constructed only through create(), so an analyzer can never run with a
/// non-positive duration, a radius outside (0, 1], a max factor below 1, or a
/// negative click weight.
class AutoZoomParameters final {
public:
    /// Defaults, documented as the single source of truth for the tuning.
    ///
    /// minDwellDuration (800 ms): the cursor must stay focused within
    ///   focusRadius for at least this long before a zoom-in is proposed. It is
    ///   both the detection threshold and the minimum span a purely
    ///   dwell-driven candidate can have.
    /// focusRadius (0.08): the normalized activity radius that separates a
    ///   "focused" cursor from a "roaming" one. A move within this distance of
    ///   the running dwell centroid keeps the dwell alive; a move beyond it ends
    ///   the dwell (zoom back out).
    /// maxZoomFactor (2.5): the tightest zoom the heuristic will ever propose.
    ///   Tighter dwells map toward this cap; loose ones stay closer to 1.
    /// minGap (500 ms): candidates whose time ranges sit closer than this are
    ///   merged into one, so the eventual playback does not rapidly zoom out and
    ///   straight back in.
    /// clickWeight (0.15 per click): how much each click inside a dwell adds to
    ///   its score, and the lever that lets a tight click cluster qualify a
    ///   dwell too short to qualify on time alone.
    static constexpr core::DurationNs kDefaultMinDwellDuration =
        std::chrono::duration_cast<core::DurationNs>(std::chrono::milliseconds{800});
    static constexpr double kDefaultFocusRadius = 0.08;
    static constexpr double kDefaultMaxZoomFactor = 2.5;
    static constexpr core::DurationNs kDefaultMinGap =
        std::chrono::duration_cast<core::DurationNs>(std::chrono::milliseconds{500});
    static constexpr double kDefaultClickWeight = 0.15;

    /// Fails with InvalidArgument if minDwellDuration is not positive, if
    /// focusRadius is not finite or not in (0, 1], if maxZoomFactor is not
    /// finite or below 1, if minGap is negative, or if clickWeight is negative
    /// or not finite.
    [[nodiscard]] static core::Result<AutoZoomParameters> create(
        core::DurationNs minDwellDuration = kDefaultMinDwellDuration,
        double focusRadius = kDefaultFocusRadius,
        double maxZoomFactor = kDefaultMaxZoomFactor,
        core::DurationNs minGap = kDefaultMinGap,
        double clickWeight = kDefaultClickWeight) {
        if (minDwellDuration.count() <= 0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "minDwellDuration must be positive"};
        }
        if (!std::isfinite(focusRadius) || focusRadius <= 0.0 || focusRadius > 1.0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "focusRadius must be finite and within (0, 1]"};
        }
        if (!std::isfinite(maxZoomFactor) || maxZoomFactor < 1.0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "maxZoomFactor must be finite and >= 1.0"};
        }
        if (minGap.count() < 0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "minGap must not be negative"};
        }
        if (!std::isfinite(clickWeight) || clickWeight < 0.0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "clickWeight must be finite and non-negative"};
        }
        return AutoZoomParameters{minDwellDuration, focusRadius, maxZoomFactor, minGap,
                                  clickWeight};
    }

    [[nodiscard]] core::DurationNs minDwellDuration() const noexcept { return minDwellDuration_; }
    [[nodiscard]] double focusRadius() const noexcept { return focusRadius_; }
    [[nodiscard]] double maxZoomFactor() const noexcept { return maxZoomFactor_; }
    [[nodiscard]] core::DurationNs minGap() const noexcept { return minGap_; }
    [[nodiscard]] double clickWeight() const noexcept { return clickWeight_; }

    friend bool operator==(const AutoZoomParameters&, const AutoZoomParameters&) = default;

private:
    AutoZoomParameters(core::DurationNs minDwellDuration, double focusRadius,
                       double maxZoomFactor, core::DurationNs minGap,
                       double clickWeight) noexcept
        : minDwellDuration_(minDwellDuration),
          focusRadius_(focusRadius),
          maxZoomFactor_(maxZoomFactor),
          minGap_(minGap),
          clickWeight_(clickWeight) {}

    core::DurationNs minDwellDuration_;
    double focusRadius_;
    double maxZoomFactor_;
    core::DurationNs minGap_;
    double clickWeight_;
};

}  // namespace creator::autozoom
