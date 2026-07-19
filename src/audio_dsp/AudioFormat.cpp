#include "audio_dsp/AudioFormat.h"

#include "core/AppError.h"

namespace creator::audio_dsp {

core::Result<AudioFormat> AudioFormat::create(std::uint32_t sampleRateHz,
                                              std::uint32_t channelCount) {
    if (sampleRateHz == 0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "AudioFormat requires a positive sample rate"};
    }
    if (channelCount == 0 || channelCount > kMaxChannels) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "AudioFormat channel count must be within 1..256"};
    }
    return AudioFormat{sampleRateHz, channelCount};
}

}  // namespace creator::audio_dsp
