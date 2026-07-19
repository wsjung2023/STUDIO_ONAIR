#include "cut_suggest/FillerDetector.h"

#include "cut_suggest/CutReason.h"
#include "cut_suggest/CutSuggestParameters.h"
#include "transcription/TranscriptWord.h"

#include "CutSuggestTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <span>
#include <string>
#include <vector>

namespace {

using creator::core::ErrorCode;
using creator::cut_suggest::CutReason;
using creator::cut_suggest::CutSuggestParameters;
using creator::cut_suggest::FillerDetector;
using creator::transcription::TranscriptWord;
using cut_suggest_test::word;

FillerDetector defaultDetector() {
    return FillerDetector{CutSuggestParameters::create().value()};
}

std::vector<creator::cut_suggest::CutSuggestion> detect(
    const FillerDetector& detector, const std::vector<TranscriptWord>& words) {
    auto result = detector.detect(std::span<const TranscriptWord>{words});
    EXPECT_TRUE(result.hasValue());
    return result.hasValue() ? result.value()
                             : std::vector<creator::cut_suggest::CutSuggestion>{};
}

TEST(FillerDetectorTest, FlagsSingleFillerWord) {
    const std::vector<TranscriptWord> words{
        word("hello", 0, 100, 0.9),
        word("um", 200, 100, 0.8),
        word("world", 400, 100, 0.9),
    };
    const auto cuts = detect(defaultDetector(), words);
    ASSERT_EQ(cuts.size(), 1u);
    EXPECT_EQ(cuts[0].reason(), CutReason::Filler);
    ASSERT_TRUE(cuts[0].label().has_value());
    EXPECT_EQ(*cuts[0].label(), "um");
    EXPECT_EQ(cuts[0].span(), words[1].range());
    EXPECT_DOUBLE_EQ(cuts[0].score(), 0.8);
}

TEST(FillerDetectorTest, IsCaseAndPunctuationInsensitive) {
    const std::vector<TranscriptWord> words{
        word("Um,", 0, 100, 1.0),
    };
    const auto cuts = detect(defaultDetector(), words);
    ASSERT_EQ(cuts.size(), 1u);
    EXPECT_EQ(*cuts[0].label(), "um");
}

TEST(FillerDetectorTest, MatchesMultiWordFillerAsOneSuggestion) {
    const std::vector<TranscriptWord> words{
        word("so", 0, 100, 0.9),
        word("you", 200, 100, 0.8),
        word("know", 400, 100, 0.6),
        word("then", 600, 100, 0.9),
    };
    const auto cuts = detect(defaultDetector(), words);
    ASSERT_EQ(cuts.size(), 1u);
    EXPECT_EQ(*cuts[0].label(), "you know");
    // Spans from the first matched word's start to the last matched word's end.
    EXPECT_EQ(cuts[0].span().start(), words[1].range().start());
    EXPECT_EQ(cuts[0].span().end(), words[2].range().end());
    // Score is the mean confidence of the two covered words.
    EXPECT_DOUBLE_EQ(cuts[0].score(), 0.7);
}

TEST(FillerDetectorTest, CleanTranscriptYieldsNothing) {
    const std::vector<TranscriptWord> words{
        word("hello", 0, 100, 0.9),
        word("world", 200, 100, 0.9),
    };
    EXPECT_TRUE(detect(defaultDetector(), words).empty());
}

TEST(FillerDetectorTest, EmptyInputYieldsNothing) {
    EXPECT_TRUE(detect(defaultDetector(), {}).empty());
}

TEST(FillerDetectorTest, HonorsMinFillerConfidence) {
    const std::vector<TranscriptWord> words{word("um", 0, 100, 0.3)};

    // Default (min confidence 0) accepts the low-confidence filler.
    EXPECT_EQ(detect(defaultDetector(), words).size(), 1u);

    // Raising the gate above the word's confidence rejects it.
    const FillerDetector strict{CutSuggestParameters::create(
        -45.0, std::chrono::milliseconds{500}, std::chrono::milliseconds{20}, 0.5)
                                    .value()};
    EXPECT_TRUE(detect(strict, words).empty());
}

TEST(FillerDetectorTest, RejectsUnsortedWords) {
    const std::vector<TranscriptWord> words{
        word("um", 500, 100, 0.9),
        word("uh", 100, 100, 0.9),  // starts before the previous word
    };
    const auto result =
        defaultDetector().detect(std::span<const TranscriptWord>{words});
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(FillerDetectorTest, IsDeterministic) {
    const std::vector<TranscriptWord> words{
        word("um", 0, 100, 0.8),
        word("you", 200, 100, 0.8),
        word("know", 400, 100, 0.8),
    };
    EXPECT_EQ(detect(defaultDetector(), words), detect(defaultDetector(), words));
}

}  // namespace
