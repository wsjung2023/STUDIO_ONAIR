#include "audio_dsp/CompressorProcessor.h"

#include "audio_dsp/DspMath.h"
#include "core/AppError.h"

#include <algorithm>
#include <cmath>

namespace creator::audio_dsp {

namespace {

/// Level floor so a silent envelope maps to a finite, very negative dB value
/// instead of -inf (which would poison the gain computer). -240 dBFS is far
/// below anything audible, so it only affects true silence.
constexpr double kLevelFloor = 1e-12;

[[nodiscard]] double linearToDb(double linear) noexcept {
    return 20.0 * std::log10(std::max(linear, kLevelFloor));
}

[[nodiscard]] double dbToLinear(double db) noexcept {
    return std::pow(10.0, db / 20.0);
}

/// One-pole smoothing coefficient for a time constant `tau` at sample rate `fs`:
/// exp(-1/(fs*tau)). A larger coefficient (closer to 1) means slower tracking.
[[nodiscard]] double smoothingCoeff(double fs, double tauSeconds) noexcept {
    return std::exp(-1.0 / (fs * tauSeconds));
}

/// Static gain-computer: given the detected input level in dB, returns the
/// downward gain reduction in dB (<= 0) for a soft-knee compressor. Continuous
/// (value and slope) across the whole curve, so there is no discontinuity at the
/// threshold even as the knee width goes to zero.
[[nodiscard]] double gainReductionDb(double levelDb, double thresholdDb,
                                     double ratio, double kneeWidthDb) noexcept {
    const double over = levelDb - thresholdDb;
    const double slope = (1.0 / ratio) - 1.0;  // <= 0
    if (kneeWidthDb > 0.0 && 2.0 * over > -kneeWidthDb &&
        2.0 * over < kneeWidthDb) {
        // Quadratic knee: a parabola joining the unity and compressed segments
        // with matching value and slope at both knee edges.
        const double t = over + kneeWidthDb / 2.0;
        return slope * (t * t) / (2.0 * kneeWidthDb);
    }
    if (2.0 * over <= -kneeWidthDb) {
        return 0.0;  // below the knee: untouched
    }
    return slope * over;  // above the knee: T + over/ratio, i.e. slope*over of GR
}

}  // namespace

CompressorProcessor::CompressorProcessor(const Parameters& params) noexcept
    : params_(params) {}

core::Result<CompressorProcessor> CompressorProcessor::create(
    const Parameters& params) {
    if (!std::isfinite(params.thresholdDb)) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "CompressorProcessor threshold must be finite"};
    }
    if (!std::isfinite(params.ratio) || params.ratio < 1.0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "CompressorProcessor ratio must be finite and >= 1"};
    }
    if (!std::isfinite(params.kneeWidthDb) || params.kneeWidthDb < 0.0) {
        return core::AppError{
            core::ErrorCode::InvalidArgument,
            "CompressorProcessor knee width must be finite and >= 0"};
    }
    if (params.attack.count() <= 0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "CompressorProcessor attack must be positive"};
    }
    if (params.release.count() <= 0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "CompressorProcessor release must be positive"};
    }
    if (!std::isfinite(params.makeupGainDb)) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "CompressorProcessor makeup gain must be finite"};
    }
    return CompressorProcessor{params};
}

core::Result<void> CompressorProcessor::process(AudioBuffer& buffer) {
    if (buffer.empty()) {
        return core::ok();  // valid no-op
    }

    // Refuse a buffer with NaN/Inf rather than smear it through the detector
    // and gain (CLAUDE.md §5). Scan first so an error leaves the buffer intact.
    for (const float s : buffer.samples()) {
        if (!std::isfinite(s)) {
            return core::AppError{
                core::ErrorCode::InvalidArgument,
                "CompressorProcessor received a non-finite input sample"};
        }
    }

    const double fs = static_cast<double>(buffer.format().sampleRateHz());
    const double attackSec =
        std::chrono::duration<double>(params_.attack).count();
    const double releaseSec =
        std::chrono::duration<double>(params_.release).count();
    const double attackCoeff = smoothingCoeff(fs, attackSec);
    const double releaseCoeff = smoothingCoeff(fs, releaseSec);
    const double makeupLinear = dbToLinear(params_.makeupGainDb);
    const bool rms = params_.detection == Detection::Rms;

    const std::uint32_t channels = buffer.channelCount();
    const std::size_t frames = buffer.frameCount();
    for (std::size_t frame = 0; frame < frames; ++frame) {
        float* const base = buffer.data() + frame * channels;

        // Linked detector key: the loudest channel drives the shared gain so the
        // multichannel image is preserved (RMS keys on power, peak on amplitude).
        double key = 0.0;
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            const double v = static_cast<double>(base[ch]);
            key = std::max(key, rms ? v * v : std::abs(v));
        }

        // Envelope follower: fast attack when rising, slow release when falling.
        const double coeff = (key > envelope_) ? attackCoeff : releaseCoeff;
        // Flush the persisted envelope so a long decay toward silence cannot
        // leave a subnormal in the feedback path (denormal CPU stall — §9).
        envelope_ = flushDenorm(coeff * envelope_ + (1.0 - coeff) * key);

        const double level = rms ? std::sqrt(envelope_) : envelope_;
        const double grDb = gainReductionDb(linearToDb(level), params_.thresholdDb,
                                            params_.ratio, params_.kneeWidthDb);
        // The envelope is already smoothed, so the gain moves continuously
        // sample to sample (no zipper); makeup is a fixed post-gain.
        gainLinear_ = dbToLinear(grDb);
        const float total = static_cast<float>(gainLinear_ * makeupLinear);

        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            base[ch] *= total;
        }
    }

    return core::ok();
}

void CompressorProcessor::reset() noexcept {
    envelope_ = 0.0;
    gainLinear_ = 1.0;
}

}  // namespace creator::audio_dsp
