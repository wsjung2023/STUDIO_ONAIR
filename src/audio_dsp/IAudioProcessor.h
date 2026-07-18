#pragma once

#include "audio_dsp/AudioBuffer.h"
#include "core/Result.h"

#include <cstddef>

namespace creator::audio_dsp {

/// Port for one node in the audio processing chain.
///
/// A processor transforms a block of PCM in place and returns whether it
/// succeeded, so nodes compose: the same AudioBuffer view is handed from one
/// processor to the next with no copy. Implementations must be deterministic
/// (no clock, no RNG-with-time — CLAUDE.md §5) and must not let exceptions
/// cross this boundary (CLAUDE.md §4): failure is reported through Result, not
/// thrown. An empty buffer is a valid no-op input.
class IAudioProcessor {
public:
    virtual ~IAudioProcessor() = default;

    /// Transform `buffer` in place. On error the buffer contents are
    /// unspecified and the caller must stop the chain.
    [[nodiscard]] virtual core::Result<void> process(AudioBuffer& buffer) = 0;

    /// Frames of latency this node adds between an input frame and the output
    /// frame carrying it. Zero-latency nodes (gain, compressor) keep the
    /// default; a look-ahead node (limiter) overrides it. A composite chain sums
    /// these so the surrounding pipeline can compensate for A/V sync
    /// (CLAUDE.md §5). Non-pure so existing processors need no change.
    [[nodiscard]] virtual std::size_t latencyFrames() const noexcept { return 0; }

protected:
    IAudioProcessor() = default;
    IAudioProcessor(const IAudioProcessor&) = default;
    IAudioProcessor(IAudioProcessor&&) = default;
    IAudioProcessor& operator=(const IAudioProcessor&) = default;
    IAudioProcessor& operator=(IAudioProcessor&&) = default;
};

}  // namespace creator::audio_dsp
