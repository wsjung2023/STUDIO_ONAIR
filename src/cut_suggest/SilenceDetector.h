#pragma once

#include "audio_dsp/AudioBuffer.h"
#include "core/Result.h"
#include "cut_suggest/CutSuggestParameters.h"
#include "cut_suggest/CutSuggestion.h"

#include <vector>

namespace creator::cut_suggest {

/// Proposes SILENCE cut suggestions from recorded PCM. Pure, deterministic,
/// offline (CLAUDE.md 8): no clock, no RNG, no I/O, no Qt/FFmpeg/MLT — the same
/// samples in always yield the same suggestions out.
///
/// Algorithm (documented so the output is explainable):
///   1. Slice the interleaved float32 buffer into consecutive RMS windows of
///      rmsWindow duration (the trailing partial window is measured over
///      whatever frames remain). Each window's level is the root-mean-square of
///      all its samples across every channel.
///   2. A window whose RMS is below silenceThresholdDbfs (converted to a linear
///      amplitude) is "silent". A maximal run of consecutive silent windows
///      whose total duration is at least minSilenceDuration becomes one
///      CutSuggestion{Silence} spanning that run.
///   3. The score blends how LONG the run is (vs 2x the minimum) with how FAR
///      BELOW the threshold it sits (a deeper-than-threshold, longer run scores
///      higher), clamped to [0, 1].
///
/// Timebase: the span is expressed on the project timebase (CLAUDE.md 2.3) with
/// the buffer's first sample at time 0. The caller is responsible for handing in
/// audio whose sample 0 aligns with the project origin its transcript uses; the
/// analyzer documents this shared-origin assumption.
///
/// Sample rate: any positive sample rate carried by the AudioBuffer's format is
/// accepted (there is no 48 kHz requirement here); the window and minimum-run
/// lengths are derived from that rate so the thresholds stay in real time.
///
/// DEFERRED (not here): the actual non-destructive CUT that removes the span,
/// ripple/gap handling, and the review UI (editor + edit-engine, Codex R1
/// territory). This class only proposes.
class SilenceDetector final {
public:
    explicit SilenceDetector(CutSuggestParameters parameters)
        : parameters_(std::move(parameters)) {}

    /// Scans the buffer and returns time-ordered, non-overlapping silence
    /// suggestions. An empty buffer is NOT an error: it yields an empty list.
    /// Fails with InvalidArgument if the buffer contains a non-finite sample
    /// (NaN/infinity), which would make an RMS meaningless.
    [[nodiscard]] core::Result<std::vector<CutSuggestion>> detect(
        const audio_dsp::AudioBuffer& audio) const;

private:
    CutSuggestParameters parameters_;
};

}  // namespace creator::cut_suggest
