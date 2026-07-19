#include "audio_dsp/AudioCleanupChain.h"

#include <memory>
#include <utility>

namespace creator::audio_dsp {

core::Result<std::unique_ptr<AudioProcessingChain>> makeAudioCleanupChain(
    const AudioFormat& format, std::unique_ptr<IAudioProcessor> denoise,
    const AudioCleanupParameters& params) {
    // Dynamics stages first — surface a bad configuration as the node's own
    // create() error rather than building a half-chain (CLAUDE.md §5/§9).
    core::Result<CompressorProcessor> compressor =
        CompressorProcessor::create(params.compressor);
    if (!compressor.hasValue()) {
        return compressor.error();
    }
    core::Result<LimiterProcessor> limiter =
        LimiterProcessor::create(params.limiter, format);
    if (!limiter.hasValue()) {
        return limiter.error();
    }

    auto chain = std::make_unique<AudioProcessingChain>();
    // Reserved ML-denoise slot at the FRONT (index 0). A null node is ignored by
    // AudioProcessingChain::add, so omitting denoise leaves compressor->limiter.
    chain->add(std::move(denoise));
    chain->add(std::make_unique<CompressorProcessor>(std::move(compressor).value()));
    chain->add(std::make_unique<LimiterProcessor>(std::move(limiter).value()));
    return chain;
}

}  // namespace creator::audio_dsp
