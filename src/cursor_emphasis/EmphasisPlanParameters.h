#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "cursor_emphasis/ClickEmphasis.h"

#include <chrono>
#include <cmath>

namespace creator::cursor_emphasis {

/// Tunable, validated knobs for the emphasis-plan heuristic.
///
/// Every field carries a unit-typed value (std::chrono for time, normalized
/// fractions for space) so a caller can never pass a bare "600" and mean the
/// wrong thing (CLAUDE.md §4). The documented defaults below are the ones
/// EmphasisPlanner uses when a caller does not override them; they are tuned for
/// screen-recording cursor telemetry sampled at tens of Hz.
///
/// Constructed only through create(), so a planner can never run with a
/// non-positive click duration, a radius outside (0, 1], a non-positive idle
/// threshold, or a min-movement radius outside (0, 1].
class EmphasisPlanParameters final {
public:
    /// Defaults, documented as the single source of truth for the tuning.
    ///
    /// clickEmphasisDuration (600 ms): how long each click's emphasis is shown.
    ///   Long enough to read as a deliberate beat, short enough not to linger.
    /// emphasisRadius (0.06): the normalized radius of the click emphasis, i.e.
    ///   how large the ripple/highlight is relative to the frame.
    /// clickEmphasisStyle (Ripple): the treatment every generated directive asks
    ///   for; a human can retarget individual directives in the editor.
    /// idleThreshold (2 s): the cursor must stay within minMovementRadius of its
    ///   settling point for at least this long before the span counts as idle
    ///   (and thus a hide span is proposed).
    /// minMovementRadius (0.01): the normalized distance a sample must travel
    ///   from the idle anchor to count as a real "move". Anything closer is
    ///   jitter and does NOT reset the idle timer.
    static constexpr core::DurationNs kDefaultClickEmphasisDuration =
        std::chrono::duration_cast<core::DurationNs>(std::chrono::milliseconds{600});
    static constexpr double kDefaultEmphasisRadius = 0.06;
    static constexpr EmphasisStyle kDefaultClickEmphasisStyle = EmphasisStyle::Ripple;
    static constexpr core::DurationNs kDefaultIdleThreshold =
        std::chrono::duration_cast<core::DurationNs>(std::chrono::seconds{2});
    static constexpr double kDefaultMinMovementRadius = 0.01;

    /// Fails with InvalidArgument if clickEmphasisDuration is not positive, if
    /// emphasisRadius is not finite or not in (0, 1], if idleThreshold is not
    /// positive, or if minMovementRadius is not finite or not in (0, 1].
    [[nodiscard]] static core::Result<EmphasisPlanParameters> create(
        core::DurationNs clickEmphasisDuration = kDefaultClickEmphasisDuration,
        double emphasisRadius = kDefaultEmphasisRadius,
        EmphasisStyle clickEmphasisStyle = kDefaultClickEmphasisStyle,
        core::DurationNs idleThreshold = kDefaultIdleThreshold,
        double minMovementRadius = kDefaultMinMovementRadius) {
        if (clickEmphasisDuration.count() <= 0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "clickEmphasisDuration must be positive"};
        }
        if (!std::isfinite(emphasisRadius) || emphasisRadius <= 0.0 ||
            emphasisRadius > 1.0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "emphasisRadius must be finite and within (0, 1]"};
        }
        if (idleThreshold.count() <= 0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "idleThreshold must be positive"};
        }
        if (!std::isfinite(minMovementRadius) || minMovementRadius <= 0.0 ||
            minMovementRadius > 1.0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "minMovementRadius must be finite and within (0, 1]"};
        }
        return EmphasisPlanParameters{clickEmphasisDuration, emphasisRadius,
                                      clickEmphasisStyle, idleThreshold,
                                      minMovementRadius};
    }

    [[nodiscard]] core::DurationNs clickEmphasisDuration() const noexcept {
        return clickEmphasisDuration_;
    }
    [[nodiscard]] double emphasisRadius() const noexcept { return emphasisRadius_; }
    [[nodiscard]] EmphasisStyle clickEmphasisStyle() const noexcept {
        return clickEmphasisStyle_;
    }
    [[nodiscard]] core::DurationNs idleThreshold() const noexcept { return idleThreshold_; }
    [[nodiscard]] double minMovementRadius() const noexcept { return minMovementRadius_; }

    friend bool operator==(const EmphasisPlanParameters&,
                           const EmphasisPlanParameters&) = default;

private:
    EmphasisPlanParameters(core::DurationNs clickEmphasisDuration, double emphasisRadius,
                           EmphasisStyle clickEmphasisStyle, core::DurationNs idleThreshold,
                           double minMovementRadius) noexcept
        : clickEmphasisDuration_(clickEmphasisDuration),
          emphasisRadius_(emphasisRadius),
          clickEmphasisStyle_(clickEmphasisStyle),
          idleThreshold_(idleThreshold),
          minMovementRadius_(minMovementRadius) {}

    core::DurationNs clickEmphasisDuration_;
    double emphasisRadius_;
    EmphasisStyle clickEmphasisStyle_;
    core::DurationNs idleThreshold_;
    double minMovementRadius_;
};

}  // namespace creator::cursor_emphasis
