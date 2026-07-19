#pragma once

#include "audio_dsp/IAudioProcessor.h"
#include "core/Result.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace creator::audio_dsp {

/// Composite processor: an ordered pipeline of IAudioProcessor nodes.
///
/// The chain is itself an IAudioProcessor, so it composes recursively and can be
/// dropped anywhere a single node is expected. `process` runs each node in order
/// over the same in-place AudioBuffer and stops at the FIRST error, returning it
/// unchanged — a downstream node never runs on a buffer an upstream node failed
/// to produce (CLAUDE.md §5: never hide a processing failure). `latencyFrames`
/// is the SUM of the nodes' latencies, so a look-ahead node anywhere in the
/// chain is reported to the surrounding pipeline for A/V sync compensation
/// (CLAUDE.md §5).
///
/// Canonical R2-06 voice/broadcast order (documented here so the assembly site
/// stays consistent):
///
///   [ML denoise — DEFERRED, licensing gate] -> Compressor -> Limiter
///
/// with loudness normalization (GainProcessor) applied AROUND this real-time
/// chain by the offline LoudnessNormalizer rather than as a streaming node. The
/// ML-denoise slot is intentionally empty for now: no ML node is constructed
/// until its model/runtime licensing clears the OSS BOM (CLAUDE.md §7). When it
/// lands it goes at the FRONT (index 0), ahead of the compressor, so the
/// dynamics stages see already-cleaned audio.
///
/// Ownership is unique (CLAUDE.md §4: no raw owning pointer): the chain owns its
/// nodes and is move-only. Deterministic and exception-free at the boundary as
/// long as its nodes are.
class AudioProcessingChain final : public IAudioProcessor {
public:
    AudioProcessingChain() = default;

    /// Construct from a ready-made ordered node list (takes ownership).
    explicit AudioProcessingChain(
        std::vector<std::unique_ptr<IAudioProcessor>> nodes) noexcept
        : nodes_(std::move(nodes)) {}

    /// Builder-style append. Null nodes are ignored so a failed `create` that
    /// yielded no node cannot smuggle a null into the pipeline. Returns *this
    /// for chaining: `chain.add(a).add(b)`.
    AudioProcessingChain& add(std::unique_ptr<IAudioProcessor> node);

    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
    [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }

    /// Run every node in order over `buffer`, in place. Stops and returns the
    /// first node's error; on success returns ok. An empty chain is the
    /// identity (leaves the buffer untouched).
    [[nodiscard]] core::Result<void> process(AudioBuffer& buffer) override;

    /// Sum of the nodes' latencies, in frames. Zero for an empty chain or a
    /// chain of only zero-latency nodes.
    [[nodiscard]] std::size_t latencyFrames() const noexcept override;

private:
    std::vector<std::unique_ptr<IAudioProcessor>> nodes_;
};

}  // namespace creator::audio_dsp
