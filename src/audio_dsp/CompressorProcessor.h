#pragma once

#include "audio_dsp/IAudioProcessor.h"
#include "core/Result.h"

#include <chrono>

namespace creator::audio_dsp {

/// Downward dynamic-range compressor node in the audio processing chain.
///
/// Above the threshold the signal is attenuated toward the threshold by the
/// configured ratio; below it the signal passes untouched. A level detector
/// (peak or RMS) tracks the input envelope with exponential attack/release time
/// constants derived from the buffer's sample rate, a soft-knee gain-computer
/// turns that envelope into a target gain reduction, and the applied gain is
/// smoothed per sample so a level change never introduces a click (zipper).
///
/// Multi-channel operation is *linked* (stereo-linked): one shared gain,
/// computed from the loudest channel's envelope, is applied to every channel so
/// the stereo/multichannel image is preserved instead of drifting when one
/// channel is louder. The node is deterministic — no clock, no RNG — and never
/// lets an exception cross the IAudioProcessor boundary: invalid parameters are
/// rejected by the validating `create` factory and non-finite input is rejected
/// through Result (CLAUDE.md §4/§5). The compressor introduces no latency.
class CompressorProcessor final : public IAudioProcessor {
public:
    /// How the sidechain measures the input level driving gain reduction.
    enum class Detection {
        Peak,  ///< Instantaneous linked peak (max |sample| across channels).
        Rms,   ///< Smoothed linked power (root-mean-square) — a gentler detector.
    };

    /// Validated configuration. Time constants are typed (std::chrono), never
    /// bare integers (CLAUDE.md §4).
    struct Parameters {
        double thresholdDb = -18.0;   ///< Knee centre, dBFS.
        double ratio = 4.0;           ///< Input:output slope above the knee, >= 1.
        double kneeWidthDb = 6.0;     ///< Total soft-knee width in dB, >= 0 (0 = hard).
        std::chrono::nanoseconds attack{std::chrono::milliseconds{10}};
        std::chrono::nanoseconds release{std::chrono::milliseconds{100}};
        double makeupGainDb = 0.0;    ///< Fixed post-gain applied after compression.
        Detection detection = Detection::Peak;
    };

    /// The only way to obtain a CompressorProcessor. Rejects a ratio < 1, a
    /// negative knee width, a non-positive attack/release, and any non-finite
    /// parameter with InvalidArgument.
    [[nodiscard]] static core::Result<CompressorProcessor> create(
        const Parameters& params);

    [[nodiscard]] const Parameters& parameters() const noexcept { return params_; }

    /// Current linked detector envelope (linear). Exposed for metering and for
    /// verifying the detector settles to exactly zero on silence (denormal
    /// flush, CLAUDE.md §9).
    [[nodiscard]] double envelope() const noexcept { return envelope_; }

    /// Clear the detector envelope and applied gain back to a clean start, so
    /// the next block is processed as if by a freshly created instance (no
    /// leaked tail state across an edit/seek boundary).
    void reset() noexcept;

    [[nodiscard]] core::Result<void> process(AudioBuffer& buffer) override;

private:
    explicit CompressorProcessor(const Parameters& params) noexcept;

    Parameters params_;

    // Detector state carried across process() calls so envelopes stay
    // continuous across buffer boundaries.
    double envelope_ = 0.0;      ///< Linked level detector output (linear).
    double gainLinear_ = 1.0;    ///< Last applied compression gain (linear, <= 1).
};

}  // namespace creator::audio_dsp
