#pragma once

#include "audio_dsp/AudioBuffer.h"
#include "core/Result.h"
#include "cut_suggest/CutSuggestParameters.h"
#include "cut_suggest/CutSuggestion.h"
#include "cut_suggest/FillerDetector.h"
#include "cut_suggest/SilenceDetector.h"
#include "transcription/Transcript.h"
#include "transcription/TranscriptWord.h"

#include <span>
#include <vector>

namespace creator::cut_suggest {

/// Runs the silence and filler detectors over one recording and returns a single
/// ordered list of cut suggestions. Pure, deterministic, offline (CLAUDE.md 8):
/// identical (audio, words) input always yields an identical suggestion list.
///
/// Ordering & overlap rule (documented so the output is predictable):
///   - The result is the UNION of the silence and filler suggestions, sorted by
///     span start on the project timebase. Ties (equal start) are broken with
///     Filler before Silence, so a labelled filler is presented first.
///   - Same-reason suggestions never overlap (each detector already emits
///     non-overlapping runs). Cross-reason overlaps (a filler word sitting
///     inside or beside a low-energy region) are INTENTIONALLY preserved as
///     independent, separately-acceptable suggestions; resolving/deduplicating
///     them when a cut is actually applied is the DEFERRED editor's job.
///
/// Timebase: silence spans are measured from the audio buffer's first sample and
/// filler spans from the transcript words; the caller must supply audio and
/// transcript that share the same project-timebase origin (CLAUDE.md 2.3) for
/// the merged ordering to be meaningful.
///
/// DEFERRED (not here, do NOT build): the non-destructive CUT that applies these
/// suggestions to a timeline, ripple/gap handling, and the review UI — that is
/// the editor + edit-engine (Codex R1 territory). This class only proposes.
class CutSuggestionAnalyzer final {
public:
    explicit CutSuggestionAnalyzer(CutSuggestParameters parameters)
        : parameters_(std::move(parameters)),
          silence_(parameters_),
          filler_(parameters_) {}

    /// Analyzes recorded audio against an ordered word span. The words must be
    /// sorted non-decreasing by range start; an out-of-order span fails with
    /// InvalidArgument (delegated to the filler detector). Empty inputs yield an
    /// empty list, which is not an error.
    [[nodiscard]] core::Result<std::vector<CutSuggestion>> analyze(
        const audio_dsp::AudioBuffer& audio,
        std::span<const transcription::TranscriptWord> words) const;

    /// Convenience overload taking a whole Transcript; flattens its words (always
    /// ordered by the Transcript invariants) and delegates.
    [[nodiscard]] core::Result<std::vector<CutSuggestion>> analyze(
        const audio_dsp::AudioBuffer& audio,
        const transcription::Transcript& transcript) const;

private:
    CutSuggestParameters parameters_;
    SilenceDetector silence_;
    FillerDetector filler_;
};

}  // namespace creator::cut_suggest
