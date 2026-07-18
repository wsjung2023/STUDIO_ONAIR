#pragma once

#include "audio_dsp/AudioFormat.h"
#include "core/Result.h"

#include <array>
#include <cstdint>
#include <vector>

namespace creator::audio_dsp {

/// BS.1770-4 K-weighting pre-filter: a two-stage biquad cascade applied per
/// channel ahead of the loudness mean-square measurement.
///
/// Stage 1 is the high-shelf "pre-filter" (~+4 dB HF), stage 2 the RLB
/// high-pass, using the exact 48 kHz coefficients from ITU-R BS.1770-4. The
/// filter keeps independent Direct-Form-I state for every channel and carries
/// it across process calls, so a signal fed one block at a time is filtered
/// identically to the same signal fed whole (continuous across buffer edges).
///
/// The coefficients are only valid at 48 kHz, so `create` requires that rate.
/// TODO(r2-06): recompute stage coefficients via the bilinear transform (or
/// resample to 48 kHz first) to support other sample rates.
class KWeightingFilter final {
public:
    // Stage 1 (high-shelf) coefficients, normalised by a0 (ITU-R BS.1770-4, 48 kHz).
    static constexpr double kStage1B0 = 1.53512485958697;
    static constexpr double kStage1B1 = -2.69169618940638;
    static constexpr double kStage1B2 = 1.19839281085285;
    static constexpr double kStage1A1 = -1.69065929318241;
    static constexpr double kStage1A2 = 0.73248077421585;

    // Stage 2 (RLB high-pass) coefficients, normalised by a0.
    static constexpr double kStage2B0 = 1.0;
    static constexpr double kStage2B1 = -2.0;
    static constexpr double kStage2B2 = 1.0;
    static constexpr double kStage2A1 = -1.99004745483398;
    static constexpr double kStage2A2 = 0.99007225036621;

    /// Validates the 48 kHz precondition and allocates per-channel state.
    [[nodiscard]] static core::Result<KWeightingFilter> create(
        const AudioFormat& format);

    /// K-weight one input sample on `channel`, advancing that channel's cascade
    /// state; returns the filtered output sample.
    [[nodiscard]] float processSample(std::uint32_t channel, float input) noexcept;

    /// Clear all channel states back to zero (silence history).
    void reset() noexcept;

    /// Largest magnitude held anywhere in the persisted biquad state, across
    /// every channel and both stages. Exposed to verify the feedback state
    /// settles to exactly zero on silence rather than lingering as a subnormal
    /// (denormal flush, CLAUDE.md §9).
    [[nodiscard]] double maxAbsState() const noexcept;

    [[nodiscard]] std::uint32_t channelCount() const noexcept {
        return channelCount_;
    }

private:
    /// Direct-Form-I state for one biquad on one channel.
    struct BiquadState {
        double x1 = 0.0;
        double x2 = 0.0;
        double y1 = 0.0;
        double y2 = 0.0;
    };

    explicit KWeightingFilter(std::uint32_t channelCount);

    std::uint32_t channelCount_;
    // Two cascade stages per channel: [channel][stage].
    std::vector<std::array<BiquadState, 2>> state_;
};

}  // namespace creator::audio_dsp
