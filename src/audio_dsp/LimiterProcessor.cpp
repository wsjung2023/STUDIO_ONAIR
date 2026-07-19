#include "audio_dsp/LimiterProcessor.h"

#include "audio_dsp/DspMath.h"
#include "core/AppError.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace creator::audio_dsp {

namespace {

[[nodiscard]] double dbToLinear(double db) noexcept {
    return std::pow(10.0, db / 20.0);
}

// 4x oversampling true-peak FIR, matching LoudnessMeter's inter-sample peak
// path (a windowed-sinc polyphase interpolator). Sharing the design keeps the
// limiter's estimate consistent with the loudness meter that verifies it.
// TODO(r2-06): factor this into a shared TruePeakOversampler if a third user
// appears, and swap for the exact BS.1770 Annex 2 table if bit-exact dBTP is
// ever required.
constexpr int kOversampleFactor = 4;
constexpr int kTapsPerPhase = 12;
constexpr int kProtoLength = kOversampleFactor * kTapsPerPhase;  // 48

[[nodiscard]] std::vector<double> buildOversampleTaps() {
    std::vector<double> taps(static_cast<std::size_t>(kProtoLength), 0.0);
    const double center = (kProtoLength - 1) / 2.0;
    constexpr double fc = 0.125;  // original Nyquist in the 4x domain
    double sum = 0.0;
    for (int n = 0; n < kProtoLength; ++n) {
        const double x = static_cast<double>(n) - center;
        const double arg = 2.0 * std::numbers::pi * fc * x;
        const double sinc = (std::abs(x) < 1e-12) ? 1.0 : std::sin(arg) / arg;
        const double lp = 2.0 * fc * sinc;
        const double window =
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) /
                                 (kProtoLength - 1));
        taps[static_cast<std::size_t>(n)] = lp * window;
        sum += lp * window;
    }
    const double scale = (sum != 0.0) ? kOversampleFactor / sum : 1.0;
    for (double& t : taps) {
        t *= scale;
    }
    return taps;
}

}  // namespace

LimiterProcessor::LimiterProcessor(const Parameters& params,
                                   const AudioFormat& format)
    : params_(params),
      format_(format),
      lookAheadFrames_(std::max<std::size_t>(
          1, static_cast<std::size_t>(std::llround(
                 std::chrono::duration<double>(params.lookAhead).count() *
                 static_cast<double>(format.sampleRateHz()))))),
      // The oversampled detector underestimates the real inter-sample peak by
      // up to ~0.7 dB near Nyquist; a positive safety margin lowers the target
      // so strict dBTP compliance has headroom. Default 0 keeps behaviour
      // unchanged.
      ceilingLinear_(dbToLinear(params.ceilingDbtp -
                                std::max(0.0, params.oversampleSafetyMarginDb))),
      // Linear attack: from unity, gain can reach zero within the look-ahead
      // window, so the ramp completes exactly as an offending peak arrives.
      attackStep_(1.0 / static_cast<double>(lookAheadFrames_)),
      releaseCoeff_(std::exp(
          -1.0 / (static_cast<double>(format.sampleRateHz()) *
                  std::chrono::duration<double>(params.release).count()))),
      // Ring spans the whole look-ahead window: L delayed samples plus the
      // newest, so emitting the oldest knows every requirement ahead of it.
      audioDelay_(format.channelCount(),
                  std::vector<float>(lookAheadFrames_ + 1, 0.0F)),
      requiredGainRing_(lookAheadFrames_ + 1, 1.0),
      truePeakHistory_(
          format.channelCount(),
          std::vector<double>(static_cast<std::size_t>(kTapsPerPhase), 0.0)),
      oversampleTaps_(buildOversampleTaps()) {}

core::Result<LimiterProcessor> LimiterProcessor::create(
    const Parameters& params, const AudioFormat& format) {
    if (!std::isfinite(params.ceilingDbtp) || params.ceilingDbtp > 0.0) {
        return core::AppError{
            core::ErrorCode::InvalidArgument,
            "LimiterProcessor ceiling must be finite and <= 0 dBTP"};
    }
    if (params.lookAhead.count() <= 0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "LimiterProcessor look-ahead must be positive"};
    }
    if (params.release.count() <= 0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "LimiterProcessor release must be positive"};
    }
    if (!std::isfinite(params.oversampleSafetyMarginDb) ||
        params.oversampleSafetyMarginDb < 0.0) {
        return core::AppError{
            core::ErrorCode::InvalidArgument,
            "LimiterProcessor oversample safety margin must be finite and >= 0"};
    }
    return LimiterProcessor{params, format};
}

double LimiterProcessor::detectTruePeak(double sample, std::size_t channel) {
    std::vector<double>& hist = truePeakHistory_[channel];
    for (std::size_t k = hist.size() - 1; k > 0; --k) {
        hist[k] = hist[k - 1];
    }
    hist[0] = sample;

    double peak = std::abs(sample);
    for (int phase = 0; phase < kOversampleFactor; ++phase) {
        double acc = 0.0;
        for (int k = 0; k < kTapsPerPhase; ++k) {
            acc += oversampleTaps_[static_cast<std::size_t>(
                       kOversampleFactor * k + phase)] *
                   hist[static_cast<std::size_t>(k)];
        }
        peak = std::max(peak, std::abs(acc));
    }
    return peak;
}

core::Result<void> LimiterProcessor::process(AudioBuffer& buffer) {
    if (buffer.empty()) {
        return core::ok();  // valid no-op
    }
    if (buffer.format() != format_) {
        return core::AppError{
            core::ErrorCode::InvalidArgument,
            "LimiterProcessor buffer format does not match the created format"};
    }

    // Scan for NaN/Inf first so a rejected buffer leaves both the samples and
    // the delay-line state untouched (CLAUDE.md §5).
    for (const float s : buffer.samples()) {
        if (!std::isfinite(s)) {
            return core::AppError{
                core::ErrorCode::InvalidArgument,
                "LimiterProcessor received a non-finite input sample"};
        }
    }

    const std::uint32_t channels = buffer.channelCount();
    const std::size_t frames = buffer.frameCount();
    const std::size_t ringSize = requiredGainRing_.size();

    for (std::size_t frame = 0; frame < frames; ++frame) {
        float* const base = buffer.data() + frame * channels;

        // Linked true-peak detection: every channel advances its own FIR
        // history, and the loudest drives one shared gain (imaging preserved).
        double truePeak = 0.0;
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            truePeak = std::max(truePeak,
                                detectTruePeak(static_cast<double>(base[ch]), ch));
        }
        const double requiredGain =
            (truePeak > ceilingLinear_) ? ceilingLinear_ / truePeak : 1.0;

        // Advance the look-ahead ring: newest at ringPos_, oldest (the sample
        // now leaving the delay line) at the next slot.
        ringPos_ = (ringPos_ + 1) % ringSize;
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            audioDelay_[ch][ringPos_] = base[ch];
        }
        requiredGainRing_[ringPos_] = requiredGain;
        const std::size_t oldest = (ringPos_ + 1) % ringSize;

        // Target = the minimum requirement anywhere in the look-ahead window, so
        // an incoming peak pulls the gain down L samples before it is emitted.
        const double windowMin = *std::min_element(requiredGainRing_.begin(),
                                                   requiredGainRing_.end());
        if (windowMin < smoothGain_) {
            smoothGain_ = std::max(windowMin, smoothGain_ - attackStep_);
        } else {
            // One-pole release toward the window requirement. Flush so the
            // persisted smoothing state can never linger as a subnormal
            // (denormal CPU stall — CLAUDE.md §9).
            smoothGain_ = flushDenorm(releaseCoeff_ * smoothGain_ +
                                      (1.0 - releaseCoeff_) * windowMin);
        }

        // Hard guarantee: never exceed the emitted sample's own requirement, so
        // even if the smoother lags, the output true-peak cannot overshoot.
        const double applied = std::min(smoothGain_, requiredGainRing_[oldest]);
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            base[ch] = static_cast<float>(audioDelay_[ch][oldest] * applied);
        }
    }

    return core::ok();
}

void LimiterProcessor::reset() noexcept {
    for (auto& channel : audioDelay_) {
        std::fill(channel.begin(), channel.end(), 0.0F);
    }
    std::fill(requiredGainRing_.begin(), requiredGainRing_.end(), 1.0);
    ringPos_ = 0;
    for (auto& channel : truePeakHistory_) {
        std::fill(channel.begin(), channel.end(), 0.0);
    }
    smoothGain_ = 1.0;
}

}  // namespace creator::audio_dsp
