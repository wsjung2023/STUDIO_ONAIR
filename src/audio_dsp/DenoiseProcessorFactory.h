#pragma once

#include "audio_dsp/IAudioProcessor.h"
#include "audio_dsp/UnavailableDenoiseProcessor.h"

#include <memory>

namespace creator::audio_dsp {

/// Denoise-node selection for the reserved ML-denoise slot (AudioProcessingChain
/// index 0).
///
/// The codebase's Unavailable pattern is a compile-time selection between a real
/// adapter node and an Unavailable fallback, chosen at the assembly site
/// (see how src/main.cpp picks mlt_adapter::MltEditEngine vs
/// edit_engine::UnavailableEditEngine under CS_APP_ENABLE_MLT). The denoise slot
/// mirrors that exactly:
///
///   * OFF (default): `makeUnavailableDenoiseProcessor()` yields the node below,
///     which errors clearly on process(). It links no RNNoise code and keeps
///     cs_audio_dsp Qt-free and rnnoise-free, so the default build and its unit
///     tests exercise it.
///   * ON (CS_ENABLE_RNNOISE): the assembly site instead calls
///     `rnnoise_adapter::createRnnoiseDenoiseProcessor(runtimeRoot)` (declared in
///     the enabled-only adapter), which verifies the audited runtime manifest
///     and returns the real RnnoiseDenoiseProcessor as an IAudioProcessor.
///
/// Keeping the two factories in their own layers is deliberate: the real one
/// cannot live here without pulling RNNoise into cs_audio_dsp, which the
/// Qt-free/rnnoise-free boundary forbids.
[[nodiscard]] inline std::unique_ptr<IAudioProcessor>
makeUnavailableDenoiseProcessor() {
    return std::make_unique<UnavailableDenoiseProcessor>();
}

}  // namespace creator::audio_dsp
