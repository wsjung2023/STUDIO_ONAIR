#pragma once

#include "core/Result.h"
#include "cut_suggest/CutSuggestParameters.h"
#include "cut_suggest/CutSuggestion.h"
#include "transcription/Transcript.h"
#include "transcription/TranscriptWord.h"

#include <span>
#include <vector>

namespace creator::cut_suggest {

/// Proposes FILLER cut suggestions from a transcript's words. Pure,
/// deterministic, offline (CLAUDE.md 8): the same words in always yield the same
/// suggestions out.
///
/// Algorithm (documented so the output is explainable):
///   1. Each word is normalized (lowercased, surrounding punctuation stripped —
///      see TextNormalize.h) so "Um," and "um" compare equal.
///   2. Sweeping left to right, at each position the LONGEST lexicon phrase that
///      matches the run of words starting there wins (so the two-word "you know"
///      is preferred over matching "you" alone). A match becomes one
///      CutSuggestion{Filler} spanning from the first matched word's start to the
///      last matched word's end, labelled with the matched phrase, and the
///      matched words are consumed before scanning resumes.
///   3. A match only fires if every word it covers has recognizer confidence at
///      least minFillerConfidence; otherwise the detector falls back to a
///      shorter match (or none) at that position. The score is the match
///      certainty (1.0 for an exact normalized match) times the mean confidence
///      of the covered words.
///
/// DEFERRED (not here): the actual non-destructive CUT that removes the word,
/// and the review UI (editor + edit-engine, Codex R1 territory). This class only
/// proposes.
class FillerDetector final {
public:
    explicit FillerDetector(CutSuggestParameters parameters)
        : parameters_(std::move(parameters)) {}

    /// Scans an ordered word span and returns time-ordered, non-overlapping
    /// filler suggestions. The words must be sorted non-decreasing by their
    /// range start on the project timebase; an out-of-order span fails with
    /// InvalidArgument rather than producing a misleading suggestion. An empty
    /// span (or an empty lexicon) yields an empty list, which is not an error.
    [[nodiscard]] core::Result<std::vector<CutSuggestion>> detect(
        std::span<const transcription::TranscriptWord> words) const;

    /// Convenience overload: flattens a Transcript's segment words (already
    /// guaranteed globally ordered by the Transcript invariants) and delegates
    /// to the span overload.
    [[nodiscard]] core::Result<std::vector<CutSuggestion>> detect(
        const transcription::Transcript& transcript) const;

private:
    CutSuggestParameters parameters_;
};

/// Flattens a transcript's per-segment words into one ordered vector. Exposed so
/// the analyzer can share the exact same flattening the filler detector uses.
[[nodiscard]] std::vector<transcription::TranscriptWord> flattenWords(
    const transcription::Transcript& transcript);

}  // namespace creator::cut_suggest
