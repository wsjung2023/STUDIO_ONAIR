#pragma once

#include "audio_dsp/AudioFormat.h"
#include "audio_dsp/AudioProcessingChain.h"
#include "audio_dsp/CompressorProcessor.h"
#include "audio_dsp/IAudioProcessor.h"
#include "audio_dsp/LimiterProcessor.h"
#include "core/Result.h"

#include <memory>

namespace creator::audio_dsp {

/// Tunables for the standard voice/broadcast cleanup chain. The defaults are the
/// nodes' own defaults (a gentle -18 dBFS / 4:1 compressor and a -1 dBTP
/// true-peak limiter), so the common case is `makeAudioCleanupChain(format)`.
struct AudioCleanupParameters {
    CompressorProcessor::Parameters compressor{};  ///< Downward dynamics.
    LimiterProcessor::Parameters limiter{};        ///< True-peak safety ceiling.
};

/// Factory for the canonical R2-06 real-time cleanup chain:
///
///   [ denoise? ] -> Compressor -> true-peak Limiter
///
/// This is the ONE place the standard cleanup order is assembled, so every call
/// site (preview, export) gets an identical, consistent chain rather than
/// hand-building the node list and risking drift (CLAUDE.md §5's documented
/// canonical order lives in AudioProcessingChain's comment; this realizes it).
///
/// `denoise` is the reserved ML-denoise slot: pass a real denoise node (e.g.
/// rnnoise_adapter::createRnnoiseDenoiseProcessor(...)) to prepend it at the
/// FRONT so the dynamics stages see already-cleaned audio, or pass nullptr (the
/// default) to omit it — which is exactly the default (gate-OFF) build, where no
/// ML node is linked. A null node is simply not added; it never smuggles a null
/// into the pipeline.
///
/// Loudness normalization ("음량 표준화") is deliberately NOT a node here: it is
/// an offline, two-pass operation applied AROUND this real-time chain by
/// LoudnessNormalizer / ExportLoudnessAnalyzer, not a streaming node (a static
/// program gain cannot be known per-block).
///
/// The compressor is format-agnostic, but the true-peak limiter is fixed to
/// `format` (sample rate for its look-ahead delay, channel count for its
/// per-channel state), so the returned chain must be fed buffers of that exact
/// format. Returns the compressor's/limiter's create() error unchanged if a
/// parameter is invalid, so a bad config is never silently dropped.
[[nodiscard]] core::Result<std::unique_ptr<AudioProcessingChain>>
makeAudioCleanupChain(const AudioFormat& format,
                      std::unique_ptr<IAudioProcessor> denoise = nullptr,
                      const AudioCleanupParameters& params = {});

}  // namespace creator::audio_dsp
