#include "cut_suggest/CutReason.h"
#include "cut_suggest/CutSuggestParameters.h"
#include "cut_suggest/CutSuggestion.h"

#include "CutSuggestTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {

using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::cut_suggest::CutReason;
using creator::cut_suggest::CutSuggestion;
using creator::cut_suggest::CutSuggestParameters;
using creator::cut_suggest::toString;
using cut_suggest_test::range;

// ---- CutSuggestion ----

TEST(CutSuggestionTest, AcceptsValidSilence) {
    const auto result =
        CutSuggestion::create(range(0, 1000), CutReason::Silence, 0.5);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().reason(), CutReason::Silence);
    EXPECT_DOUBLE_EQ(result.value().score(), 0.5);
    EXPECT_FALSE(result.value().label().has_value());
}

TEST(CutSuggestionTest, AcceptsValidFillerWithLabel) {
    const auto result =
        CutSuggestion::create(range(0, 1000), CutReason::Filler, 0.9, "um");
    ASSERT_TRUE(result.hasValue());
    ASSERT_TRUE(result.value().label().has_value());
    EXPECT_EQ(*result.value().label(), "um");
}

TEST(CutSuggestionTest, RejectsScoreAboveOne) {
    const auto result =
        CutSuggestion::create(range(0, 1000), CutReason::Silence, 1.5);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(CutSuggestionTest, RejectsNonFiniteScore) {
    const auto result = CutSuggestion::create(
        range(0, 1000), CutReason::Silence,
        std::numeric_limits<double>::quiet_NaN());
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(CutSuggestionTest, RejectsEmptyLabel) {
    const auto result =
        CutSuggestion::create(range(0, 1000), CutReason::Filler, 0.5, std::string{});
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(CutReasonTest, StableWireForms) {
    EXPECT_EQ(toString(CutReason::Silence), "silence");
    EXPECT_EQ(toString(CutReason::Filler), "filler");
}

// ---- CutSuggestParameters ----

TEST(CutSuggestParametersTest, DefaultsAreValidAndDocumented) {
    const auto result = CutSuggestParameters::create();
    ASSERT_TRUE(result.hasValue());
    const auto& p = result.value();
    EXPECT_DOUBLE_EQ(p.silenceThresholdDbfs(), -45.0);
    EXPECT_EQ(p.minSilenceDuration(),
              std::chrono::duration_cast<DurationNs>(std::chrono::milliseconds{500}));
    EXPECT_EQ(p.rmsWindow(),
              std::chrono::duration_cast<DurationNs>(std::chrono::milliseconds{20}));
    // Default lexicon includes single- and multi-word fillers.
    EXPECT_EQ(p.maxFillerTokens(), 2u);
}

TEST(CutSuggestParametersTest, NormalizesLexiconCaseAndPunctuation) {
    const auto result = CutSuggestParameters::create(
        -45.0, std::chrono::milliseconds{500}, std::chrono::milliseconds{20}, 0.0,
        std::vector<std::string>{"Um", "You Know!"});
    ASSERT_TRUE(result.hasValue());
    const auto& lex = result.value().fillerLexicon();
    ASSERT_EQ(lex.size(), 2u);
    EXPECT_EQ(lex[0], "um");
    EXPECT_EQ(lex[1], "you know");
}

TEST(CutSuggestParametersTest, RejectsNonNegativeThreshold) {
    const auto result = CutSuggestParameters::create(0.0);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(CutSuggestParametersTest, RejectsNonPositiveMinSilence) {
    const auto result = CutSuggestParameters::create(
        -45.0, std::chrono::milliseconds{0});
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(CutSuggestParametersTest, RejectsNonPositiveWindow) {
    const auto result = CutSuggestParameters::create(
        -45.0, std::chrono::milliseconds{500}, std::chrono::milliseconds{-1});
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(CutSuggestParametersTest, RejectsConfidenceOutOfRange) {
    const auto result = CutSuggestParameters::create(
        -45.0, std::chrono::milliseconds{500}, std::chrono::milliseconds{20}, 1.5);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(CutSuggestParametersTest, RejectsAllPunctuationLexiconEntry) {
    const auto result = CutSuggestParameters::create(
        -45.0, std::chrono::milliseconds{500}, std::chrono::milliseconds{20}, 0.0,
        std::vector<std::string>{"!!!"});
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(CutSuggestParametersTest, EmptyLexiconIsAllowed) {
    const auto result = CutSuggestParameters::create(
        -45.0, std::chrono::milliseconds{500}, std::chrono::milliseconds{20}, 0.0,
        std::vector<std::string>{});
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().maxFillerTokens(), 0u);
    EXPECT_TRUE(result.value().fillerLexicon().empty());
}

}  // namespace
