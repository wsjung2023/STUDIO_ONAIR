#include "cut_suggest/CutSuggestionAnalyzer.h"

#include "audio_dsp/AudioBuffer.h"
#include "audio_dsp/AudioFormat.h"
#include "cut_suggest/CutReason.h"
#include "cut_suggest/CutSuggestParameters.h"
#include "domain/Identifiers.h"
#include "transcription/Transcript.h"
#include "transcription/TranscriptSegment.h"
#include "transcription/TranscriptWord.h"

#include "CutSuggestTestSupport.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <vector>

namespace {

using creator::audio_dsp::AudioBuffer;
using creator::audio_dsp::AudioFormat;
using creator::core::ErrorCode;
using creator::cut_suggest::CutReason;
using creator::cut_suggest::CutSuggestionAnalyzer;
using creator::cut_suggest::CutSuggestParameters;
using creator::domain::SourceId;
using creator::transcription::Transcript;
using creator::transcription::TranscriptSegment;
using creator::transcription::TranscriptWord;
using cut_suggest_test::range;
using cut_suggest_test::word;

constexpr std::uint32_t kRate = 48'000;
constexpr float kLoud = 0.3f;

AudioFormat monoFormat() { return AudioFormat::create(kRate, 1).value(); }
std::size_t framesForMs(std::int64_t ms) {
    return static_cast<std::size_t>(kRate * ms / 1000);
}

CutSuggestionAnalyzer defaultAnalyzer() {
    return CutSuggestionAnalyzer{CutSuggestParameters::create().value()};
}

// A recording with a 600 ms silent gap (200 ms..800 ms).
std::vector<float> gappedPcm() {
    std::vector<float> pcm;
    cut_suggest_test::appendLevel(pcm, framesForMs(200), kLoud);
    cut_suggest_test::appendLevel(pcm, framesForMs(600), 0.0f);
    cut_suggest_test::appendLevel(pcm, framesForMs(200), kLoud);
    return pcm;
}

TEST(CutSuggestionAnalyzerTest, MergesAndOrdersBothReasons) {
    std::vector<float> pcm = gappedPcm();
    const AudioFormat format = monoFormat();
    AudioBuffer buffer{pcm.data(), pcm.size(), format};

    // A filler word after the gap, so the expected order is silence then filler.
    const std::vector<TranscriptWord> words{
        word("um", 900'000'000, 50'000'000, 0.8),
    };

    const auto result =
        defaultAnalyzer().analyze(buffer, std::span<const TranscriptWord>{words});
    ASSERT_TRUE(result.hasValue());
    const auto& cuts = result.value();
    ASSERT_EQ(cuts.size(), 2u);
    EXPECT_EQ(cuts[0].reason(), CutReason::Silence);
    EXPECT_EQ(cuts[1].reason(), CutReason::Filler);
    // Globally time-ordered by span start.
    EXPECT_LE(cuts[0].span().start().time_since_epoch().count(),
              cuts[1].span().start().time_since_epoch().count());
}

TEST(CutSuggestionAnalyzerTest, EmptyInputsYieldEmpty) {
    const AudioFormat format = monoFormat();
    AudioBuffer buffer{nullptr, 0, format};
    const auto result =
        defaultAnalyzer().analyze(buffer, std::span<const TranscriptWord>{});
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().empty());
}

TEST(CutSuggestionAnalyzerTest, RejectsUnsortedWords) {
    const AudioFormat format = monoFormat();
    AudioBuffer buffer{nullptr, 0, format};
    const std::vector<TranscriptWord> words{
        word("um", 500, 100, 0.9),
        word("uh", 100, 100, 0.9),
    };
    const auto result =
        defaultAnalyzer().analyze(buffer, std::span<const TranscriptWord>{words});
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(CutSuggestionAnalyzerTest, TranscriptOverloadFlattensWords) {
    const AudioFormat format = monoFormat();
    AudioBuffer buffer{nullptr, 0, format};

    std::vector<TranscriptWord> segWords{word("um", 0, 100, 0.9)};
    const auto segment =
        TranscriptSegment::create("um", range(0, 100), std::move(segWords)).value();
    const auto transcript =
        Transcript::create({segment}, "en", SourceId::create("cam-1").value()).value();

    const auto result = defaultAnalyzer().analyze(buffer, transcript);
    ASSERT_TRUE(result.hasValue());
    ASSERT_EQ(result.value().size(), 1u);
    EXPECT_EQ(result.value()[0].reason(), CutReason::Filler);
}

TEST(CutSuggestionAnalyzerTest, IsDeterministic) {
    std::vector<float> pcm = gappedPcm();
    const AudioFormat format = monoFormat();
    AudioBuffer buffer{pcm.data(), pcm.size(), format};
    const std::vector<TranscriptWord> words{word("um", 900'000'000, 50'000'000, 0.8)};

    const auto a =
        defaultAnalyzer().analyze(buffer, std::span<const TranscriptWord>{words});
    const auto b =
        defaultAnalyzer().analyze(buffer, std::span<const TranscriptWord>{words});
    ASSERT_TRUE(a.hasValue());
    ASSERT_TRUE(b.hasValue());
    EXPECT_EQ(a.value(), b.value());
}

}  // namespace
