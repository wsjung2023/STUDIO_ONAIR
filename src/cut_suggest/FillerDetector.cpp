#include "cut_suggest/FillerDetector.h"

#include "core/AppError.h"
#include "cut_suggest/TextNormalize.h"
#include "domain/TimelineTypes.h"
#include "transcription/TranscriptSegment.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <utility>

namespace creator::cut_suggest {

using core::AppError;
using core::DurationNs;
using core::ErrorCode;
using core::Result;
using core::TimestampNs;
using domain::TimeRange;
using transcription::Transcript;
using transcription::TranscriptWord;

namespace {

// Words must already be ordered on the project timebase; an out-of-order span is
// a caller/mapping bug, not a recoverable input (mirrors AutoZoomAnalyzer).
[[nodiscard]] bool isNonDecreasing(
    std::span<const TranscriptWord> words) noexcept {
    for (std::size_t i = 1; i < words.size(); ++i) {
        if (words[i].range().start() < words[i - 1].range().start()) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<TranscriptWord> flattenWords(const Transcript& transcript) {
    std::vector<TranscriptWord> words;
    for (const auto& segment : transcript.segments()) {
        for (const TranscriptWord& word : segment.words()) {
            words.push_back(word);
        }
    }
    return words;
}

Result<std::vector<CutSuggestion>> FillerDetector::detect(
    std::span<const TranscriptWord> words) const {
    if (!isNonDecreasing(words)) {
        return AppError{ErrorCode::InvalidArgument,
                        "transcript words must be sorted by timestamp"};
    }

    std::vector<CutSuggestion> suggestions;
    const std::size_t maxTokens = parameters_.maxFillerTokens();
    if (maxTokens == 0 || words.empty()) {
        return suggestions;  // empty lexicon or no words: nothing to match
    }

    const std::unordered_set<std::string> lexicon(
        parameters_.fillerLexicon().begin(), parameters_.fillerLexicon().end());
    const double minConfidence = parameters_.minFillerConfidence();

    // Precompute each word's normalized token once.
    std::vector<std::string> normalized;
    normalized.reserve(words.size());
    for (const TranscriptWord& word : words) {
        normalized.push_back(text_normalize::normalizeToken(word.text()));
    }

    const std::size_t n = words.size();
    std::size_t i = 0;
    while (i < n) {
        bool matched = false;
        const std::size_t maxLen = std::min(maxTokens, n - i);
        for (std::size_t len = maxLen; len >= 1; --len) {
            // Build the candidate phrase from the run's normalized tokens.
            std::string phrase;
            double confidenceSum = 0.0;
            bool confidenceOk = true;
            for (std::size_t k = 0; k < len; ++k) {
                if (k != 0) phrase.push_back(' ');
                phrase += normalized[i + k];
                const double c = words[i + k].confidence();
                confidenceSum += c;
                if (c < minConfidence) confidenceOk = false;
            }
            if (lexicon.find(phrase) == lexicon.end()) {
                continue;  // not a filler phrase at this length
            }
            if (!confidenceOk) {
                continue;  // matched, but too uncertain: try a shorter phrase
            }

            const TimestampNs start = words[i].range().start();
            const TimestampNs end = words[i + len - 1].range().end();
            const DurationNs duration = end - start;
            const double meanConfidence =
                confidenceSum / static_cast<double>(len);
            const double score = std::clamp(meanConfidence, 0.0, 1.0);

            auto span = TimeRange::create(start, duration);
            if (!span) return span.error();
            auto suggestion =
                CutSuggestion::create(span.value(), CutReason::Filler, score, phrase);
            if (!suggestion) return suggestion.error();
            suggestions.push_back(std::move(suggestion).value());

            i += len;
            matched = true;
            break;
        }
        if (!matched) ++i;
    }

    return suggestions;
}

Result<std::vector<CutSuggestion>> FillerDetector::detect(
    const Transcript& transcript) const {
    const std::vector<TranscriptWord> words = flattenWords(transcript);
    return detect(std::span<const TranscriptWord>{words});
}

}  // namespace creator::cut_suggest
