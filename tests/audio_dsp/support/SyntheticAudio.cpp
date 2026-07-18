#include "audio_dsp/support/SyntheticAudio.h"

#include <cmath>
#include <numbers>

namespace creator::audio_dsp::testing {

AudioSignal makeSilence(const AudioFormat& format, std::size_t frameCount) {
    return AudioSignal{format, frameCount};  // vector is zero-initialised
}

AudioSignal makeSine(const AudioFormat& format, std::size_t frameCount,
                     double frequencyHz, double levelDbfs) {
    AudioSignal signal{format, frameCount};
    const double amplitude = std::pow(10.0, levelDbfs / 20.0);
    const double radiansPerFrame = 2.0 * std::numbers::pi * frequencyHz /
                                   static_cast<double>(format.sampleRateHz());
    const std::uint32_t channels = format.channelCount();
    std::vector<float>& out = signal.samples();
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        const double value =
            amplitude * std::sin(radiansPerFrame * static_cast<double>(frame));
        const float s = static_cast<float>(value);
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            out[frame * channels + ch] = s;
        }
    }
    return signal;
}

}  // namespace creator::audio_dsp::testing
