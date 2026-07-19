#include "audio_dsp/KWeightingFilter.h"

#include "audio_dsp/DspMath.h"
#include "core/AppError.h"

#include <algorithm>
#include <cmath>

namespace creator::audio_dsp {

namespace {

/// The BS.1770 coefficients above are derived for exactly this rate; using them
/// at any other rate would silently mis-measure loudness (CLAUDE.md §5: no
/// silent audio damage), so the meter requires it.
constexpr std::uint32_t kRequiredSampleRateHz = 48'000;

}  // namespace

core::Result<KWeightingFilter> KWeightingFilter::create(const AudioFormat& format) {
    if (format.sampleRateHz() != kRequiredSampleRateHz) {
        return core::AppError{
            core::ErrorCode::InvalidArgument,
            "KWeightingFilter coefficients are defined only for 48 kHz"};
    }
    return KWeightingFilter{format.channelCount()};
}

KWeightingFilter::KWeightingFilter(std::uint32_t channelCount)
    : channelCount_(channelCount), state_(channelCount) {}

float KWeightingFilter::processSample(std::uint32_t channel, float input) noexcept {
    // Caller precondition: channel < channelCount_. Guard defensively so a bad
    // index cannot read past the state vector.
    if (channel >= channelCount_) {
        return 0.0F;
    }
    // One Direct-Form-I biquad step with a0 already folded into the
    // coefficients. Local so it can touch the private BiquadState.
    const auto step = [](BiquadState& s, double x, double b0, double b1,
                         double b2, double a1, double a2) noexcept -> double {
        const double y = b0 * x + b1 * s.x1 + b2 * s.x2 - a1 * s.y1 - a2 * s.y2;
        s.x2 = s.x1;
        s.x1 = x;
        // Flush persisted feedback state so the slow RLB pole (a2 ~ 0.99007)
        // cannot leave a subnormal lingering after silence (denormal CPU
        // stall — CLAUDE.md §9).
        s.y2 = flushDenorm(s.y1);
        s.y1 = flushDenorm(y);
        return y;
    };

    std::array<BiquadState, 2>& cascade = state_[channel];
    const double stage1 = step(cascade[0], static_cast<double>(input), kStage1B0,
                               kStage1B1, kStage1B2, kStage1A1, kStage1A2);
    const double stage2 = step(cascade[1], stage1, kStage2B0, kStage2B1,
                               kStage2B2, kStage2A1, kStage2A2);
    return static_cast<float>(stage2);
}

void KWeightingFilter::reset() noexcept {
    for (auto& channel : state_) {
        channel = {};
    }
}

double KWeightingFilter::maxAbsState() const noexcept {
    double peak = 0.0;
    for (const auto& channel : state_) {
        for (const BiquadState& s : channel) {
            peak = std::max({peak, std::abs(s.x1), std::abs(s.x2),
                             std::abs(s.y1), std::abs(s.y2)});
        }
    }
    return peak;
}

}  // namespace creator::audio_dsp
