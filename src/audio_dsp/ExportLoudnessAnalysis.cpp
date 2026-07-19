#include "audio_dsp/ExportLoudnessAnalysis.h"

#include "audio_dsp/AudioBuffer.h"
#include "audio_dsp/LoudnessMeter.h"
#include "audio_dsp/LoudnessNormalizer.h"
#include "core/AppError.h"

#include <cmath>
#include <cstddef>

namespace creator::audio_dsp {

core::Result<ExportLoudnessAnalyzer> ExportLoudnessAnalyzer::create(
    const Parameters& params) {
    // Reuse LoudnessNormalizer's parameter validation verbatim so the analyzer
    // and the normalizer can never disagree on what a valid target/ceiling is.
    LoudnessNormalizer::Parameters normParams;
    normParams.targetLufs = params.targetLufs;
    normParams.truePeakCeilingDbtp = params.truePeakCeilingDbtp;
    if (core::Result<LoudnessNormalizer> validated =
            LoudnessNormalizer::create(normParams);
        !validated.hasValue()) {
        return validated.error();
    }
    return ExportLoudnessAnalyzer{params};
}

core::Result<ExportLoudnessDecision> ExportLoudnessAnalyzer::analyze(
    const std::vector<float>& interleaved, const AudioFormat& format) const {
    // PASS 1 measurement: one sweep of a fresh LoudnessMeter over the whole
    // program. Propagate the meter's errors (non-48 kHz format, non-finite
    // sample) unchanged so the decision never hides a measurement failure.
    core::Result<LoudnessMeter> meter = LoudnessMeter::create(format);
    if (!meter.hasValue()) {
        return meter.error();
    }

    const std::size_t channels = format.channelCount();
    const std::size_t frames = channels == 0 ? 0 : interleaved.size() / channels;
    // A const_cast is safe here: AudioBuffer is a mutable view type, but
    // LoudnessMeter::addBlock only reads it. The samples are not modified.
    AudioBuffer view{const_cast<float*>(interleaved.data()), frames, format};
    if (core::Result<void> added = meter.value().addBlock(view);
        !added.hasValue()) {
        return added.error();
    }

    const double measuredLufs = meter.value().integratedLufs();
    const double truePeak = meter.value().truePeakDbtp();

    // Near-silence guard, identical to LoudnessNormalizer: no valid measurement
    // (silence / too short) or below the noise floor is a documented no-op —
    // boosting it would only amplify noise.
    if (!std::isfinite(measuredLufs) ||
        measuredLufs < LoudnessNormalizer::kNoiseFloorLufs) {
        return ExportLoudnessDecision{measuredLufs,
                                      truePeak,
                                      params_.targetLufs,
                                      params_.truePeakCeilingDbtp,
                                      0.0,
                                      false};
    }

    // The single static gain that lands the program on target — the exact value
    // LoudnessNormalizer's pass 2 would apply.
    const double gainDb = params_.targetLufs - measuredLufs;
    return ExportLoudnessDecision{measuredLufs,
                                  truePeak,
                                  params_.targetLufs,
                                  params_.truePeakCeilingDbtp,
                                  gainDb,
                                  true};
}

}  // namespace creator::audio_dsp
