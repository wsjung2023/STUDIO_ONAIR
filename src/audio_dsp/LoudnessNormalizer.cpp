#include "audio_dsp/LoudnessNormalizer.h"

#include "audio_dsp/AudioBuffer.h"
#include "audio_dsp/GainProcessor.h"
#include "audio_dsp/LimiterProcessor.h"
#include "audio_dsp/LoudnessMeter.h"
#include "core/AppError.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace creator::audio_dsp {

namespace {

/// One integrated-loudness + true-peak reading over a whole interleaved signal.
struct Measurement {
    double integratedLufs;
    double truePeakDbtp;
};

/// Run a fresh LoudnessMeter over `interleaved` at `format`. Propagates the
/// meter's errors (non-48 kHz format, non-finite sample) unchanged so the
/// normalizer never hides them.
[[nodiscard]] core::Result<Measurement> measureSignal(
    const std::vector<float>& interleaved, const AudioFormat& format) {
    core::Result<LoudnessMeter> meter = LoudnessMeter::create(format);
    if (!meter.hasValue()) {
        return meter.error();
    }

    const std::size_t channels = format.channelCount();
    const std::size_t frames = channels == 0 ? 0 : interleaved.size() / channels;
    // A const_cast is safe here: AudioBuffer is a mutable view type, but
    // LoudnessMeter::addBlock only reads. The samples are not modified.
    AudioBuffer view{const_cast<float*>(interleaved.data()), frames, format};
    if (core::Result<void> added = meter.value().addBlock(view);
        !added.hasValue()) {
        return added.error();
    }
    return Measurement{meter.value().integratedLufs(),
                       meter.value().truePeakDbtp()};
}

}  // namespace

core::Result<LoudnessNormalizer> LoudnessNormalizer::create(
    const Parameters& params) {
    if (!std::isfinite(params.targetLufs) || params.targetLufs > 0.0) {
        return core::AppError{
            core::ErrorCode::InvalidArgument,
            "LoudnessNormalizer target loudness must be finite and <= 0 LUFS"};
    }
    if (!std::isfinite(params.truePeakCeilingDbtp) ||
        params.truePeakCeilingDbtp > 0.0) {
        return core::AppError{
            core::ErrorCode::InvalidArgument,
            "LoudnessNormalizer true-peak ceiling must be finite and <= 0 dBTP"};
    }
    return LoudnessNormalizer{params};
}

core::Result<NormalizationResult> LoudnessNormalizer::normalize(
    std::vector<float>& interleaved, const AudioFormat& format) const {
    // Pass 1: measure the whole program. Any measurement error (wrong rate,
    // non-finite sample) surfaces here.
    core::Result<Measurement> before = measureSignal(interleaved, format);
    if (!before.hasValue()) {
        return before.error();
    }
    const double measuredLufs = before.value().integratedLufs;

    // Near-silence guard: no valid measurement (silence / too short) or below
    // the noise floor. Boosting would only amplify noise, so leave it untouched.
    if (!std::isfinite(measuredLufs) || measuredLufs < kNoiseFloorLufs) {
        return NormalizationResult{measuredLufs, 0.0, measuredLufs,
                                   before.value().truePeakDbtp, false};
    }

    // Pass 2a: apply the single static gain that lands the program on target.
    const double gainDb = params_.targetLufs - measuredLufs;
    GainProcessor gain{gainDb};
    {
        AudioBuffer view{interleaved.data(),
                         format.channelCount() == 0
                             ? 0
                             : interleaved.size() / format.channelCount(),
                         format};
        if (core::Result<void> r = gain.process(view); !r.hasValue()) {
            return r.error();
        }
    }

    // Pass 2b: a true-peak limiter at the ceiling guarantees the output cannot
    // exceed truePeakCeilingDbtp even if the gain pushed inter-sample peaks up.
    LimiterProcessor::Parameters limParams;
    limParams.ceilingDbtp = params_.truePeakCeilingDbtp;
    core::Result<LimiterProcessor> limiter =
        LimiterProcessor::create(limParams, format);
    if (!limiter.hasValue()) {
        return limiter.error();
    }

    const std::size_t channels = format.channelCount();
    const std::size_t frames = channels == 0 ? 0 : interleaved.size() / channels;
    const std::size_t latency = limiter.value().latencyFrames();

    {
        AudioBuffer view{interleaved.data(), frames, format};
        if (core::Result<void> r = limiter.value().process(view); !r.hasValue()) {
            return r.error();
        }
    }

    // The limiter delays output by `latency` frames (leading silence) and holds
    // the final `latency` frames in its look-ahead line. Flush the tail and
    // shift the whole thing back so the returned buffer keeps the input's length
    // and timing — an offline pass must not introduce A/V drift (CLAUDE.md §5).
    if (latency > 0 && frames > latency) {
        std::vector<float> tail(latency * channels, 0.0F);
        AudioBuffer tailView{tail.data(), latency, format};
        if (core::Result<void> r = limiter.value().process(tailView);
            !r.hasValue()) {
            return r.error();
        }
        const std::size_t shift = latency * channels;
        std::copy(interleaved.begin() + static_cast<std::ptrdiff_t>(shift),
                  interleaved.end(), interleaved.begin());
        std::copy(tail.begin(), tail.end(),
                  interleaved.end() - static_cast<std::ptrdiff_t>(shift));
    }

    // Pass 3: re-measure to report what was actually achieved.
    core::Result<Measurement> after = measureSignal(interleaved, format);
    if (!after.hasValue()) {
        return after.error();
    }
    return NormalizationResult{measuredLufs, gainDb,
                               after.value().integratedLufs,
                               after.value().truePeakDbtp, true};
}

}  // namespace creator::audio_dsp
