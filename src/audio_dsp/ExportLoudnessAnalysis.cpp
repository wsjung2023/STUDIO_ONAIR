#include "audio_dsp/ExportLoudnessAnalysis.h"

#include "audio_dsp/AudioBuffer.h"
#include "audio_dsp/LoudnessMeter.h"
#include "audio_dsp/LoudnessNormalizer.h"

#include <cmath>
#include <cstddef>

namespace creator::audio_dsp {

namespace {

ExportLoudnessDecision makeDecision(
    double measuredLufs, double truePeak,
    const ExportLoudnessAnalyzer::Parameters& params) {
    if (!std::isfinite(measuredLufs) ||
        measuredLufs < LoudnessNormalizer::kNoiseFloorLufs) {
        return ExportLoudnessDecision{measuredLufs,
                                      truePeak,
                                      params.targetLufs,
                                      params.truePeakCeilingDbtp,
                                      0.0,
                                      false};
    }

    return ExportLoudnessDecision{measuredLufs,
                                  truePeak,
                                  params.targetLufs,
                                  params.truePeakCeilingDbtp,
                                  params.targetLufs - measuredLufs,
                                  true};
}

}  // namespace

core::Result<ExportLoudnessAnalyzer> ExportLoudnessAnalyzer::create(
    const Parameters& params) {
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
    core::Result<LoudnessMeter> meter = LoudnessMeter::create(format);
    if (!meter.hasValue()) {
        return meter.error();
    }

    const std::size_t channels = format.channelCount();
    const std::size_t frames = channels == 0 ? 0 : interleaved.size() / channels;
    AudioBuffer view{const_cast<float*>(interleaved.data()), frames, format};
    if (core::Result<void> added = meter.value().addBlock(view);
        !added.hasValue()) {
        return added.error();
    }
    return decide(meter.value());
}

core::Result<ExportLoudnessDecision> ExportLoudnessAnalyzer::decide(
    const LoudnessMeter& meter) const {
    return makeDecision(
        meter.integratedLufs(), meter.truePeakDbtp(), params_);
}

}  // namespace creator::audio_dsp
