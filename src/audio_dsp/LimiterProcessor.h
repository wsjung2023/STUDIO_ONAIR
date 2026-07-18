#pragma once

#include "audio_dsp/AudioFormat.h"
#include "audio_dsp/IAudioProcessor.h"
#include "core/Result.h"

#include <chrono>
#include <cstddef>
#include <vector>

namespace creator::audio_dsp {

/// Brickwall look-ahead TRUE-PEAK limiter node.
///
/// Limits the output to the configured ceiling (dBTP) as measured by a *4x
/// oversampled* true-peak estimate — catching the inter-sample peaks a plain
/// sample meter misses by reconstructing an oversampled estimate of each input
/// frame's true peak. The signal is delayed by the look-ahead window so gain
/// reduction is fully ramped in *before* an offending peak reaches the output;
/// there is no instantaneous gain step (no click), and the applied gain is
/// additionally clamped to the exact per-sample requirement. Gain recovers over
/// the release time constant.
///
/// Honest bound (CLAUDE.md §9 — do not overstate a guarantee): 4x oversampling
/// underestimates the real inter-sample true peak by up to ~0.7 dB for
/// near-Nyquist / sharp-transient content, so the *true* dBTP can exceed the
/// ceiling by roughly that much even though the 4x estimate does not. For strict
/// dBTP compliance, set `Parameters::oversampleSafetyMarginDb` (e.g. 1.0 dB) so
/// the limiter targets `ceilingDbtp − margin` and reserves that headroom.
///
/// Channels are *linked*: a peak on any single channel attenuates all channels
/// equally, so the stereo/multichannel image is preserved.
///
/// The look-ahead costs latency: `latencyFrames()` / `latency()` report it so
/// the surrounding pipeline can compensate for A/V sync (CLAUDE.md §5). Because
/// the delay line and detector are sized from the sample rate and channel count,
/// the format is fixed at `create` time; `process` rejects a mismatched buffer.
class LimiterProcessor final : public IAudioProcessor {
public:
    /// Validated configuration. The look-ahead and release are typed
    /// (std::chrono), never bare integers (CLAUDE.md §4).
    struct Parameters {
        double ceilingDbtp = -1.0;  ///< Maximum output true-peak, dBTP (<= 0).
        std::chrono::nanoseconds lookAhead{std::chrono::microseconds{1500}};
        std::chrono::nanoseconds release{std::chrono::milliseconds{100}};
        /// Extra headroom (dB, >= 0) subtracted from the ceiling to cover the 4x
        /// oversampled detector's ~0.7 dB underestimate of the real inter-sample
        /// peak. 0 (default) keeps behaviour unchanged; e.g. 1.0 targets
        /// `ceilingDbtp − 1 dB` for strict dBTP compliance.
        double oversampleSafetyMarginDb = 0.0;
    };

    /// The only way to obtain a LimiterProcessor. Rejects a non-finite or
    /// positive ceiling, a non-positive look-ahead, a non-positive release, and
    /// a non-finite or negative oversample safety margin with InvalidArgument.
    /// `format` fixes the sample rate (for the look-ahead delay length) and
    /// channel count (for the per-channel delay lines).
    [[nodiscard]] static core::Result<LimiterProcessor> create(
        const Parameters& params, const AudioFormat& format);

    [[nodiscard]] const Parameters& parameters() const noexcept { return params_; }
    [[nodiscard]] const AudioFormat& format() const noexcept { return format_; }

    /// Latency introduced by the look-ahead, in frames. Fixed at construction.
    /// Overrides IAudioProcessor so a chain can sum node latencies generically.
    [[nodiscard]] std::size_t latencyFrames() const noexcept override {
        return lookAheadFrames_;
    }
    /// The same latency expressed as wall-clock time at the fixed sample rate.
    [[nodiscard]] std::chrono::nanoseconds latency() const noexcept {
        return format_.durationForFrames(lookAheadFrames_);
    }

    /// Clear the look-ahead delay line, gain ring, detector history and smoothed
    /// gain back to a clean start, so the next block is processed as if by a
    /// freshly created instance (no leaked tail state across an edit/seek
    /// boundary). The fixed format-derived sizing is preserved.
    void reset() noexcept;

    [[nodiscard]] core::Result<void> process(AudioBuffer& buffer) override;

private:
    LimiterProcessor(const Parameters& params, const AudioFormat& format);

    [[nodiscard]] double detectTruePeak(double sample, std::size_t channel);

    Parameters params_;
    AudioFormat format_;
    std::size_t lookAheadFrames_;
    double ceilingLinear_;
    double attackStep_;      ///< Max downward gain step/sample (linear, linear attack).
    double releaseCoeff_;    ///< One-pole release coefficient toward unity.

    // Look-ahead delay line for the audio, per channel (ring buffer).
    std::vector<std::vector<float>> audioDelay_;
    // Ring of required linear gains aligned with the audio delay, so emitting a
    // delayed sample knows the gain requirement of the whole look-ahead window.
    std::vector<double> requiredGainRing_;
    std::size_t ringPos_ = 0;

    // Oversampling FIR history per channel for the true-peak detector.
    std::vector<std::vector<double>> truePeakHistory_;
    std::vector<double> oversampleTaps_;

    double smoothGain_ = 1.0;  ///< Smoothed applied gain (linear), carried across calls.
};

}  // namespace creator::audio_dsp
