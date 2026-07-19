#pragma once

#include "audio_dsp/IAudioProcessor.h"
#include "core/Result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace creator::rnnoise_adapter {

/// Real-time RNNoise denoiser filling the reserved ML-denoise slot
/// (AudioProcessingChain index 0), implementing audio_dsp::IAudioProcessor.
///
/// RNNoise operates on fixed 480-sample (10 ms) mono frames of 48 kHz float
/// audio scaled to the int16 domain it was trained on. This node adapts our
/// interleaved, arbitrarily-sized, multi-channel float32 buffers to that:
///
///   * Framing: incoming samples are accumulated into 480-sample frames across
///     process() calls; a completed frame is denoised and its output is emitted
///     through a per-channel delay line primed with one frame of silence. That
///     gives a fixed algorithmic latency of exactly one frame (480 samples),
///     reported by latencyFrames() so the surrounding pipeline can compensate
///     for A/V sync (CLAUDE.md §5). Output stays the same size as input, in
///     place.
///   * Multi-channel: RNNoise is mono, so each channel is denoised by its OWN
///     independent DenoiseState (documented, deliberate — there is no
///     multi-channel RNNoise model). The channel count is taken from the buffer
///     and the per-channel state is (re)allocated if it changes.
///   * Scaling: samples are multiplied by 32768 before RNNoise and divided by
///     32768 after, because the shipped model is trained on int16-range values,
///     not the normalized [-1, 1] our AudioBuffer carries.
///
/// Contract (CLAUDE.md §4/§5/§9): a non-48 kHz buffer is rejected with
/// InvalidArgument; a non-finite (NaN/±∞) input sample is rejected with
/// InvalidArgument before any mutation; no exception crosses the process()
/// boundary; failures travel through Result. Ownership is RAII: DenoiseStates
/// and the fixed-size delay lines are owned and freed automatically, and the
/// delay ring is bounded (no unbounded queue). reset() clears the frame buffer,
/// re-primes the delay lines and reinitializes every DenoiseState so the next
/// block starts clean across an edit/seek boundary.
class RnnoiseDenoiseProcessor final : public audio_dsp::IAudioProcessor {
public:
    /// RNNoise's fixed frame size (rnnoise_get_frame_size() == 480) and the only
    /// sample rate it supports.
    static constexpr std::size_t kFrameSize = 480;
    static constexpr std::uint32_t kRequiredSampleRateHz = 48000;

    ~RnnoiseDenoiseProcessor() override;
    RnnoiseDenoiseProcessor(RnnoiseDenoiseProcessor&&) noexcept;
    RnnoiseDenoiseProcessor& operator=(RnnoiseDenoiseProcessor&&) noexcept;
    RnnoiseDenoiseProcessor(const RnnoiseDenoiseProcessor&) = delete;
    RnnoiseDenoiseProcessor& operator=(const RnnoiseDenoiseProcessor&) = delete;

    [[nodiscard]] core::Result<void> process(
        audio_dsp::AudioBuffer& buffer) override;

    /// One frame (480 samples) of fixed algorithmic latency from the framing +
    /// delay-line priming described above.
    [[nodiscard]] std::size_t latencyFrames() const noexcept override;

    /// Clear the frame buffer, re-prime the delay lines and reinitialize every
    /// per-channel DenoiseState. No reallocation of the channel count.
    void reset() noexcept;

private:
    RnnoiseDenoiseProcessor();

    friend core::Result<std::unique_ptr<audio_dsp::IAudioProcessor>>
    createRnnoiseDenoiseProcessor(const std::filesystem::path& runtimeRoot);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Verify the audited RNNoise runtime prefix, then build the real denoiser as an
/// IAudioProcessor. This is the CS_ENABLE_RNNOISE counterpart to
/// audio_dsp::makeUnavailableDenoiseProcessor(): the assembly site selects one
/// or the other at compile time (see audio_dsp/DenoiseProcessorFactory.h). A
/// failed manifest verification is surfaced as an error, never hidden behind a
/// silently-degraded node (CLAUDE.md §9).
[[nodiscard]] core::Result<std::unique_ptr<audio_dsp::IAudioProcessor>>
createRnnoiseDenoiseProcessor(const std::filesystem::path& runtimeRoot);

}  // namespace creator::rnnoise_adapter
