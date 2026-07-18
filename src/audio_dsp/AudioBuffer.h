#pragma once

#include "audio_dsp/AudioFormat.h"

#include <cassert>
#include <cstddef>
#include <span>

namespace creator::audio_dsp {

/// Non-owning, mutable view over one block of interleaved float32 PCM.
///
/// The buffer deliberately owns nothing (CLAUDE.md §4 RAII): the sample storage
/// lives in the caller (a std::vector in tests, a capture ring elsewhere) and
/// outlives every AudioBuffer that views it. Processors mutate samples in place
/// through this view, so the same span can flow through a chain of
/// IAudioProcessor nodes without copying. Frame-major indexing: sample
/// `frame*channelCount + channel`.
///
/// An empty view (null pointer or zero frames) is a valid, explicitly handled
/// state — processors treat it as a no-op rather than dereferencing it.
class AudioBuffer final {
public:
    /// `samples` must point at `frameCount * format.channelCount()` floats. A
    /// null pointer (or zero frames) yields an empty view.
    AudioBuffer(float* samples, std::size_t frameCount,
                const AudioFormat& format) noexcept
        : format_(format),
          frameCount_(samples == nullptr ? 0 : frameCount),
          samples_(samples,
                   samples == nullptr
                       ? std::size_t{0}
                       : frameCount * format.channelCount()) {}

    [[nodiscard]] const AudioFormat& format() const noexcept { return format_; }
    [[nodiscard]] std::size_t frameCount() const noexcept { return frameCount_; }
    [[nodiscard]] std::uint32_t channelCount() const noexcept {
        return format_.channelCount();
    }
    /// Total interleaved sample count (frames × channels).
    [[nodiscard]] std::size_t sampleCount() const noexcept {
        return samples_.size();
    }
    [[nodiscard]] bool empty() const noexcept { return frameCount_ == 0; }

    [[nodiscard]] float* data() noexcept { return samples_.data(); }
    [[nodiscard]] const float* data() const noexcept { return samples_.data(); }

    [[nodiscard]] std::span<float> samples() noexcept { return samples_; }
    [[nodiscard]] std::span<const float> samples() const noexcept {
        return samples_;
    }

    /// Frame-major sample access. Indexing out of range is a caller
    /// precondition violation (asserted), not a recoverable path.
    [[nodiscard]] float& sample(std::size_t frame, std::uint32_t channel) noexcept {
        assert(frame < frameCount_ && channel < channelCount());
        return samples_[frame * channelCount() + channel];
    }
    [[nodiscard]] float sample(std::size_t frame,
                               std::uint32_t channel) const noexcept {
        assert(frame < frameCount_ && channel < channelCount());
        return samples_[frame * channelCount() + channel];
    }

private:
    AudioFormat format_;
    std::size_t frameCount_;
    std::span<float> samples_;
};

}  // namespace creator::audio_dsp
