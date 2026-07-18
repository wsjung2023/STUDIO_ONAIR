#pragma once

#include "audio_dsp/AudioBuffer.h"
#include "audio_dsp/AudioFormat.h"

#include <cstddef>
#include <vector>

namespace creator::audio_dsp::testing {

/// Owning PCM test signal: a std::vector that actually holds the samples plus
/// the format describing them. Handing out an AudioBuffer view keeps ownership
/// here (CLAUDE.md §4 RAII) while letting processors mutate the samples in
/// place. All generators are formula-driven and deterministic — no clock, no
/// RNG — so tests can assert exact/near-exact values (CLAUDE.md §5, §8).
class AudioSignal final {
public:
    AudioSignal(AudioFormat format, std::size_t frameCount)
        : format_(format),
          samples_(frameCount * format.channelCount(), 0.0F) {}

    [[nodiscard]] const AudioFormat& format() const noexcept { return format_; }
    [[nodiscard]] std::size_t frameCount() const noexcept {
        return format_.channelCount() == 0
                   ? 0
                   : samples_.size() / format_.channelCount();
    }
    [[nodiscard]] std::vector<float>& samples() noexcept { return samples_; }
    [[nodiscard]] const std::vector<float>& samples() const noexcept {
        return samples_;
    }

    /// Mutable, non-owning view suitable for feeding an IAudioProcessor.
    [[nodiscard]] AudioBuffer view() noexcept {
        return AudioBuffer{samples_.data(), frameCount(), format_};
    }

private:
    AudioFormat format_;
    std::vector<float> samples_;
};

/// All-zero signal.
[[nodiscard]] AudioSignal makeSilence(const AudioFormat& format,
                                      std::size_t frameCount);

/// Full-scale-referenced sine at `frequencyHz` and `levelDbfs` dBFS
/// (0 dBFS = amplitude 1.0). The same waveform is written to every channel.
[[nodiscard]] AudioSignal makeSine(const AudioFormat& format,
                                   std::size_t frameCount, double frequencyHz,
                                   double levelDbfs);

}  // namespace creator::audio_dsp::testing
