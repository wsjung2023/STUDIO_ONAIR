#pragma once

#include "core/Result.h"

#include <chrono>
#include <cstdint>

namespace creator::audio_dsp {

/// Immutable description of an interleaved float32 PCM stream: how many samples
/// per second each channel carries and how many channels are interleaved.
///
/// This is a value object with an enforced invariant — it can only exist in a
/// valid state, because it is created through the validating `create` factory
/// (positive sample rate, at least one channel). The DSP layer treats every
/// AudioFormat it receives as already-valid, so the checks live here once
/// instead of at every processing node. The layout is fixed: interleaved
/// (frame-major) 32-bit float, matching what the capture adapter produces, so
/// no colour-space/endianness fields are needed at this layer.
class AudioFormat final {
public:
    /// Defensive upper bound so a corrupt channel count cannot ask a buffer to
    /// stride by billions of samples. 256 comfortably covers any real layout.
    static constexpr std::uint32_t kMaxChannels = 256;

    /// The only way to obtain an AudioFormat. Rejects a zero sample rate and a
    /// channel count outside 1..kMaxChannels with InvalidArgument.
    [[nodiscard]] static core::Result<AudioFormat> create(
        std::uint32_t sampleRateHz, std::uint32_t channelCount);

    [[nodiscard]] std::uint32_t sampleRateHz() const noexcept { return sampleRateHz_; }
    [[nodiscard]] std::uint32_t channelCount() const noexcept { return channelCount_; }

    /// Wall-clock duration of `frameCount` frames at this sample rate. Time is
    /// always typed (CLAUDE.md §4) — never a bare integer of samples.
    [[nodiscard]] std::chrono::nanoseconds durationForFrames(
        std::uint64_t frameCount) const noexcept {
        // Split into whole seconds + remainder so the intermediate
        // `frames * 1e9` never overflows u64 (it would past ~1.84e10 frames,
        // ~4.3 days at 48 kHz). Exact for all inputs.
        const std::uint64_t rate = sampleRateHz_;
        return std::chrono::nanoseconds{
            (frameCount / rate) * 1'000'000'000ULL +
            ((frameCount % rate) * 1'000'000'000ULL) / rate};
    }

    friend bool operator==(const AudioFormat&, const AudioFormat&) = default;

private:
    AudioFormat(std::uint32_t sampleRateHz, std::uint32_t channelCount) noexcept
        : sampleRateHz_(sampleRateHz), channelCount_(channelCount) {}

    std::uint32_t sampleRateHz_;
    std::uint32_t channelCount_;
};

}  // namespace creator::audio_dsp
