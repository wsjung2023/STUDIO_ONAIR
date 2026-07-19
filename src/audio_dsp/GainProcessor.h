#pragma once

#include "audio_dsp/IAudioProcessor.h"

#include <chrono>

namespace creator::audio_dsp {

/// Applies a linear gain, specified in decibels, to interleaved float PCM.
///
/// When the gain target changes the applied factor is ramped toward it over a
/// short, sample-rate-derived window instead of snapping, so a level change
/// never introduces a click (a per-sample discontinuity). The ramp state is
/// carried across process() calls, so it stays continuous across buffer
/// boundaries. The node is deterministic: identical inputs and the same target
/// history produce identical output, with no clock or RNG involved.
///
/// −∞ dB (or any sufficiently low value) collapses the linear factor to 0,
/// i.e. silence. Non-finite input samples (NaN/±∞) are rejected via Result
/// rather than propagated.
class GainProcessor final : public IAudioProcessor {
public:
    /// Default 5 ms ramp: long enough to be click-free, short enough to feel
    /// instantaneous.
    static constexpr std::chrono::milliseconds kDefaultRampDuration{5};

    explicit GainProcessor(
        double gainDb = 0.0,
        std::chrono::milliseconds rampDuration = kDefaultRampDuration) noexcept;

    /// Set a new target gain. Takes effect as a ramp on the next process().
    void setGainDb(double gainDb) noexcept;
    [[nodiscard]] double gainDb() const noexcept { return gainDb_; }

    /// Snap the applied factor to the current target, discarding any in-progress
    /// ramp, so the next block starts as if by a freshly created instance at the
    /// current gain (no leaked ramp tail across an edit/seek boundary).
    void reset() noexcept;

    [[nodiscard]] core::Result<void> process(AudioBuffer& buffer) override;

private:
    double gainDb_;
    double targetLinear_;   // linear factor we are ramping toward
    double currentLinear_;  // linear factor actually applied to the last sample
    std::chrono::milliseconds rampDuration_;
};

}  // namespace creator::audio_dsp
