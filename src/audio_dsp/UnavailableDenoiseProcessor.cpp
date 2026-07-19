#include "audio_dsp/UnavailableDenoiseProcessor.h"

#include "core/AppError.h"

namespace creator::audio_dsp {

core::Result<void> UnavailableDenoiseProcessor::process(AudioBuffer& buffer) {
    // An empty buffer is a valid no-op input (IAudioProcessor contract), so it
    // is not an error to hand this node nothing.
    if (buffer.empty()) {
        return core::ok();
    }
    return core::AppError{
        core::ErrorCode::InvalidState,
        "RNNoise denoise adapter is not built in this configuration "
        "(CS_ENABLE_RNNOISE=OFF)"};
}

}  // namespace creator::audio_dsp
