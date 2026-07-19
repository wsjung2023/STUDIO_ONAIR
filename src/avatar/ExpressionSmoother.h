#pragma once

#include "avatar/ExpressionParameters.h"
#include "core/Timebase.h"

namespace creator::avatar {

/// One-Euro adaptive low-pass filter (Casiez, Roussel, Vogel 2012) applied
/// independently to each of the nine ExpressionParameters fields, to remove
/// tracking jitter frame-to-frame. The filter is adaptive: when a signal is
/// nearly still it applies heavy smoothing (kills jitter), and when it moves
/// fast it relaxes toward low lag (so a real expression change is not felt as
/// "stuck"). This is the standard filter for face/hand tracking for exactly
/// that reason - a fixed-cutoff low-pass cannot be both jitter-free at rest
/// and responsive during motion.
///
/// Stateful and pure compute: no clock read, no sleep, no gating. `dt` is
/// derived only from the `core::TimestampNs` passed to smooth(), never from a
/// wall clock read internally (CLAUDE.md 9 forbids wall-clock-based sync);
/// callers drive time via the timestamps on their own frames, which keeps this
/// class deterministically testable with scripted timestamps.
class ExpressionSmoother final {
public:
    /// Tunable constants shared by all nine per-field filters.
    ///
    /// Defaults chosen for face-expression tracking at typical webcam frame
    /// rates (30-60fps):
    ///   - minCutoffHz = 1.0: the cutoff frequency used when the signal's
    ///     smoothed derivative is ~0 (holding still). Lower = more smoothing
    ///     at rest. 1 Hz is a common One-Euro starting point and, for
    ///     normalized [0,1]/[-1,1] expression fields, gives a clearly visible
    ///     jitter reduction without perceptible added lag on typical
    ///     expression changes.
    ///   - beta = 0.007: how strongly the cutoff rises with signal speed
    ///     (`cutoff = minCutoff + beta * |derivative|`). Expression fields
    ///     move over a total span of 1.0-2.0 units/sec during a fast
    ///     expression change (e.g. a blink or a wide-mouth open in ~150ms),
    ///     so a small beta is enough to noticeably relax the cutoff during
    ///     real motion while barely reacting to sub-jitter-sized derivative
    ///     noise. This mirrors the original paper's own small-beta guidance
    ///     for low-amplitude signals.
    ///   - dCutoffHz = 1.0: cutoff used to smooth the derivative signal
    ///     itself before it feeds the adaptive term. 1 Hz is the value the
    ///     original paper uses and rarely needs tuning.
    struct Constants final {
        double minCutoffHz{1.0};
        double beta{0.007};
        double dCutoffHz{1.0};
    };

    ExpressionSmoother();
    explicit ExpressionSmoother(Constants constants);

    /// Advances all nine per-field filters by one frame and returns the
    /// smoothed parameters.
    ///
    /// First call ever (or first call after reset()): no previous timestamp
    /// exists to compute dt from, so every field passes through unchanged
    /// (the documented One-Euro cold-start policy) and internal state is
    /// seeded from `raw`.
    ///
    /// Non-positive dt (t <= previous timestamp - a repeated or out-of-order
    /// timestamp): dividing by dt would divide by zero or invert time, so
    /// this call returns each field's *last filtered output* unchanged and
    /// does not advance the derivative state or the stored timestamp. This is
    /// a defined degradation (CLAUDE.md 9: don't hide a failure behind a
    /// plausible number) rather than a crash or a NaN.
    [[nodiscard]] ExpressionParameters smooth(const ExpressionParameters& raw,
                                              core::TimestampNs t);

    /// Clears all filter state so the next smooth() call is a fresh
    /// cold-start passthrough, exactly as if this were a brand new
    /// ExpressionSmoother. Used when starting a new tracking session so the
    /// previous session's last pose cannot bleed into the new one.
    void reset();

private:
    // Per-scalar-signal One-Euro filter state. One instance per
    // ExpressionParameters field, all nine sharing the smoother's Constants.
    // Constants are passed into step() by value rather than cached as a
    // pointer/reference to the owning ExpressionSmoother's field: that would
    // leave a copied or moved-from ExpressionSmoother's filters aliasing the
    // wrong object's Constants.
    class OneEuroFilter final {
    public:
        // dt is ignored on the first-ever call (no previous sample to
        // measure it against) and rejected (dt <= 0) on any later call with a
        // repeated or out-of-order timestamp; see ExpressionSmoother::smooth
        // for the exact contract.
        [[nodiscard]] float step(float x, double dt, const Constants& constants);
        void reset();

    private:
        bool initialized_{false};
        float xPrev_{0.0F};
        float dxPrev_{0.0F};
    };

    Constants constants_;
    bool haveTimestamp_{false};
    core::TimestampNs tPrev_{};

    OneEuroFilter eyeOpenLeft_;
    OneEuroFilter eyeOpenRight_;
    OneEuroFilter browUpLeft_;
    OneEuroFilter browUpRight_;
    OneEuroFilter mouthOpen_;
    OneEuroFilter mouthWide_;
    OneEuroFilter headYaw_;
    OneEuroFilter headPitch_;
    OneEuroFilter headRoll_;
};

}  // namespace creator::avatar
