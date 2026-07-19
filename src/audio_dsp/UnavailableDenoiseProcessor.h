#pragma once

#include "audio_dsp/IAudioProcessor.h"
#include "core/Result.h"

#include <cstddef>

namespace creator::audio_dsp {

/// Fallback node for the reserved ML-denoise slot when the audited RNNoise
/// adapter is not built (CS_ENABLE_RNNOISE=OFF, the default).
///
/// This mirrors edit_engine::UnavailableEditEngine: rather than silently
/// omitting the stage and pretending denoise happened, it surfaces a clear,
/// recoverable error the first time it is asked to process audio. An empty
/// buffer is still a valid no-op (nothing to deny). It links no RNNoise code
/// and stays Qt-free, so it lives in cs_audio_dsp and is exercised by the
/// default build's unit tests (CLAUDE.md §9: never hide that a stage is
/// missing).
///
/// Assembly sites that would rather leave the slot empty when denoise is
/// unavailable can simply not add this node — AudioProcessingChain::add drops
/// nulls and an absent node is the documented "omit the denoise slot" option.
class UnavailableDenoiseProcessor final : public IAudioProcessor {
public:
    [[nodiscard]] core::Result<void> process(AudioBuffer& buffer) override;

    /// No latency: the node never actually delays a frame, it refuses up front.
    [[nodiscard]] std::size_t latencyFrames() const noexcept override { return 0; }
};

}  // namespace creator::audio_dsp
