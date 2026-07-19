#pragma once

#include "core/AppError.h"
#include "core/Result.h"
#include "core/Timebase.h"
#include "cut_suggest/TextNormalize.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace creator::cut_suggest {

/// Tunable, validated knobs for the R2-05 cut-suggestion heuristics.
///
/// Every time field carries a unit-typed std::chrono value and every level is a
/// dBFS double, so a caller can never pass a bare "500" and mean the wrong thing
/// (CLAUDE.md 4). The documented defaults below are the ones the analyzer uses
/// when a caller does not override them; they are tuned for spoken-word screen
/// recordings.
///
/// Constructed only through create(), so a detector can never run with a
/// non-negative silence threshold, a non-positive window/duration, a filler
/// confidence outside [0, 1], or a lexicon entry that is all punctuation.
class CutSuggestParameters final {
public:
    /// Defaults, documented as the single source of truth for the tuning.
    ///
    /// silenceThresholdDbfs (-45 dBFS): an RMS window quieter than this counts
    ///   as silent. Full scale is 0 dBFS, so this must be negative.
    /// minSilenceDuration (500 ms): a run of silent windows shorter than this is
    ///   ignored — natural gaps between words are not dead air worth cutting.
    /// rmsWindow (20 ms): the analysis window the level is measured over. Short
    ///   enough to place a silence edge precisely, long enough to average out a
    ///   single quiet sample.
    /// minFillerConfidence (0.0): a transcript word whose recognizer confidence
    ///   is below this is not proposed as a filler cut. The default accepts any
    ///   confidence; raise it to trust only high-confidence recognition.
    static constexpr double kDefaultSilenceThresholdDbfs = -45.0;
    static constexpr core::DurationNs kDefaultMinSilenceDuration =
        std::chrono::duration_cast<core::DurationNs>(std::chrono::milliseconds{500});
    static constexpr core::DurationNs kDefaultRmsWindow =
        std::chrono::duration_cast<core::DurationNs>(std::chrono::milliseconds{20});
    static constexpr double kDefaultMinFillerConfidence = 0.0;

    /// The default filler lexicon: common English hesitation and filler words,
    /// including two multi-word fillers ("you know", "i mean"). Matching is
    /// case-insensitive and punctuation-insensitive (see TextNormalize.h).
    [[nodiscard]] static std::vector<std::string> defaultFillerLexicon() {
        return {"um", "uh", "er", "ah", "hmm", "like", "you know", "i mean"};
    }

    /// Fails with InvalidArgument if silenceThresholdDbfs is not finite or is
    /// >= 0, if minSilenceDuration or rmsWindow is not positive, if
    /// minFillerConfidence is not finite or outside [0, 1], or if any lexicon
    /// entry normalizes to zero tokens (empty / all punctuation). An empty
    /// lexicon is allowed and simply disables filler detection.
    [[nodiscard]] static core::Result<CutSuggestParameters> create(
        double silenceThresholdDbfs = kDefaultSilenceThresholdDbfs,
        core::DurationNs minSilenceDuration = kDefaultMinSilenceDuration,
        core::DurationNs rmsWindow = kDefaultRmsWindow,
        double minFillerConfidence = kDefaultMinFillerConfidence,
        std::vector<std::string> fillerLexicon = defaultFillerLexicon()) {
        if (!std::isfinite(silenceThresholdDbfs) || silenceThresholdDbfs >= 0.0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "silence threshold must be finite and below 0 dBFS"};
        }
        if (minSilenceDuration.count() <= 0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "minimum silence duration must be positive"};
        }
        if (rmsWindow.count() <= 0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "RMS window must be positive"};
        }
        if (!std::isfinite(minFillerConfidence) || minFillerConfidence < 0.0 ||
            minFillerConfidence > 1.0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "minimum filler confidence must be finite and within [0, 1]"};
        }

        std::vector<std::string> normalized;
        std::size_t maxTokens = 0;
        normalized.reserve(fillerLexicon.size());
        for (const std::string& entry : fillerLexicon) {
            std::vector<std::string> tokens = text_normalize::normalizePhrase(entry);
            if (tokens.empty()) {
                return core::AppError{core::ErrorCode::InvalidArgument,
                                      "filler lexicon entry must contain at least one word"};
            }
            std::string phrase;
            for (std::size_t i = 0; i < tokens.size(); ++i) {
                if (i != 0) phrase.push_back(' ');
                phrase += tokens[i];
            }
            maxTokens = std::max(maxTokens, tokens.size());
            normalized.push_back(std::move(phrase));
        }

        return CutSuggestParameters{silenceThresholdDbfs, minSilenceDuration, rmsWindow,
                                    minFillerConfidence, std::move(normalized), maxTokens};
    }

    [[nodiscard]] double silenceThresholdDbfs() const noexcept {
        return silenceThresholdDbfs_;
    }
    [[nodiscard]] core::DurationNs minSilenceDuration() const noexcept {
        return minSilenceDuration_;
    }
    [[nodiscard]] core::DurationNs rmsWindow() const noexcept { return rmsWindow_; }
    [[nodiscard]] double minFillerConfidence() const noexcept {
        return minFillerConfidence_;
    }
    /// The normalized lexicon phrases (lowercase, single-spaced), each
    /// guaranteed to hold at least one token.
    [[nodiscard]] const std::vector<std::string>& fillerLexicon() const noexcept {
        return fillerLexicon_;
    }
    /// The largest token count of any lexicon phrase (1 for a single-word-only
    /// lexicon), used to bound the multi-word match window. Zero when the
    /// lexicon is empty.
    [[nodiscard]] std::size_t maxFillerTokens() const noexcept {
        return maxFillerTokens_;
    }

    friend bool operator==(const CutSuggestParameters&,
                           const CutSuggestParameters&) = default;

private:
    CutSuggestParameters(double silenceThresholdDbfs,
                         core::DurationNs minSilenceDuration,
                         core::DurationNs rmsWindow, double minFillerConfidence,
                         std::vector<std::string> fillerLexicon,
                         std::size_t maxFillerTokens)
        : silenceThresholdDbfs_(silenceThresholdDbfs),
          minSilenceDuration_(minSilenceDuration),
          rmsWindow_(rmsWindow),
          minFillerConfidence_(minFillerConfidence),
          fillerLexicon_(std::move(fillerLexicon)),
          maxFillerTokens_(maxFillerTokens) {}

    double silenceThresholdDbfs_;
    core::DurationNs minSilenceDuration_;
    core::DurationNs rmsWindow_;
    double minFillerConfidence_;
    std::vector<std::string> fillerLexicon_;
    std::size_t maxFillerTokens_;
};

}  // namespace creator::cut_suggest
