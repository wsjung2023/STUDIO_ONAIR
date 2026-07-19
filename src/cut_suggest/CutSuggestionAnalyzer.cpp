#include "cut_suggest/CutSuggestionAnalyzer.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace creator::cut_suggest {

using core::Result;

namespace {

// Filler sorts before Silence when two suggestions share a start time, so the
// labelled, word-anchored suggestion is presented first.
[[nodiscard]] int reasonRank(CutReason reason) noexcept {
    return reason == CutReason::Filler ? 0 : 1;
}

}  // namespace

Result<std::vector<CutSuggestion>> CutSuggestionAnalyzer::analyze(
    const audio_dsp::AudioBuffer& audio,
    std::span<const transcription::TranscriptWord> words) const {
    auto silence = silence_.detect(audio);
    if (!silence) return silence.error();
    auto filler = filler_.detect(words);
    if (!filler) return filler.error();

    std::vector<CutSuggestion> merged = std::move(silence).value();
    std::vector<CutSuggestion> fillers = std::move(filler).value();
    merged.insert(merged.end(), std::make_move_iterator(fillers.begin()),
                  std::make_move_iterator(fillers.end()));

    std::stable_sort(merged.begin(), merged.end(),
                     [](const CutSuggestion& a, const CutSuggestion& b) {
                         const std::int64_t sa =
                             a.span().start().time_since_epoch().count();
                         const std::int64_t sb =
                             b.span().start().time_since_epoch().count();
                         if (sa != sb) return sa < sb;
                         return reasonRank(a.reason()) < reasonRank(b.reason());
                     });

    return merged;
}

Result<std::vector<CutSuggestion>> CutSuggestionAnalyzer::analyze(
    const audio_dsp::AudioBuffer& audio,
    const transcription::Transcript& transcript) const {
    const std::vector<transcription::TranscriptWord> words = flattenWords(transcript);
    return analyze(audio, std::span<const transcription::TranscriptWord>{words});
}

}  // namespace creator::cut_suggest
