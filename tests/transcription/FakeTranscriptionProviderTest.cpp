#include "transcription/FakeTranscriptionProvider.h"

#include "transcription/AudioInput.h"
#include "transcription/ITranscriptionProvider.h"
#include "transcription/Transcript.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using creator::core::ErrorCode;
using creator::domain::SourceId;
using creator::transcription::AudioInput;
using creator::transcription::FakeTranscriptionProvider;
using creator::transcription::TranscriptionOptions;

TranscriptionOptions options(std::string language = "en") {
    return TranscriptionOptions{SourceId::create("cam-1").value(), std::move(language)};
}

// 1000 Hz mono, 5000 frames -> exactly 5 seconds of audio.
std::vector<float> fiveSecondsMono() { return std::vector<float>(5000, 0.25f); }

std::int64_t ns(const creator::core::TimestampNs t) {
    return t.time_since_epoch().count();
}

TEST(FakeTranscriptionProviderTest, ProducesScriptedLayoutFromDuration) {
    const auto samples = fiveSecondsMono();
    const auto audio = AudioInput::create(samples, 1000, 1).value();
    FakeTranscriptionProvider provider;

    const auto result = provider.transcribe(audio, options());
    ASSERT_TRUE(result.hasValue());
    const auto& transcript = result.value();

    // 5s / 500ms = 10 words, grouped 5 per segment -> 2 segments.
    ASSERT_EQ(transcript.segments().size(), 2u);
    EXPECT_EQ(transcript.segments()[0].words().size(), 5u);
    EXPECT_EQ(transcript.segments()[1].words().size(), 5u);

    // Exact first word: "the" at [0, 400ms], confidence 1.0.
    const auto& first = transcript.segments()[0].words().front();
    EXPECT_EQ(first.text(), "the");
    EXPECT_EQ(ns(first.range().start()), 0);
    EXPECT_EQ(first.range().duration().count(), 400'000'000);
    EXPECT_DOUBLE_EQ(first.confidence(), 1.0);

    // Second word starts one 500ms slot later.
    const auto& second = transcript.segments()[0].words()[1];
    EXPECT_EQ(second.text(), "quick");
    EXPECT_EQ(ns(second.range().start()), 500'000'000);

    // Segment text is the space-joined words.
    EXPECT_EQ(transcript.segments()[0].text(), "the quick brown fox jumps");
    EXPECT_EQ(transcript.segments()[1].text(), "over lazy dog the quick");
}

TEST(FakeTranscriptionProviderTest, WordsLieWithinDurationAndAreMonotonic) {
    const auto samples = fiveSecondsMono();
    const auto audio = AudioInput::create(samples, 1000, 1).value();
    FakeTranscriptionProvider provider;
    const auto transcript = provider.transcribe(audio, options()).value();

    const std::int64_t totalNs = audio.duration().count();
    std::int64_t previousEnd = -1;
    for (const auto& segment : transcript.segments()) {
        for (const auto& word : segment.words()) {
            const std::int64_t start = ns(word.range().start());
            const std::int64_t end = start + word.range().duration().count();
            EXPECT_GE(start, previousEnd);  // strictly non-overlapping / monotonic
            EXPECT_GE(start, 0);
            EXPECT_LE(end, totalNs);
            previousEnd = end;
        }
    }
}

TEST(FakeTranscriptionProviderTest, IsDeterministic) {
    const auto samples = fiveSecondsMono();
    const auto audio = AudioInput::create(samples, 1000, 1).value();
    FakeTranscriptionProvider provider;

    const auto first = provider.transcribe(audio, options()).value();
    const auto second = provider.transcribe(audio, options()).value();
    EXPECT_TRUE(first == second);
}

TEST(FakeTranscriptionProviderTest, EchoesRequestedLanguage) {
    const auto samples = fiveSecondsMono();
    const auto audio = AudioInput::create(samples, 1000, 1).value();
    FakeTranscriptionProvider provider;

    EXPECT_EQ(provider.transcribe(audio, options("ko")).value().languageTag(), "ko");
    // Empty request defaults to a valid tag rather than producing an invalid one.
    EXPECT_EQ(provider.transcribe(audio, options("")).value().languageTag(), "en");
}

TEST(FakeTranscriptionProviderTest, AttributesSourceId) {
    const auto samples = fiveSecondsMono();
    const auto audio = AudioInput::create(samples, 1000, 1).value();
    FakeTranscriptionProvider provider;
    EXPECT_EQ(provider.transcribe(audio, options()).value().sourceId().value(), "cam-1");
}

TEST(FakeTranscriptionProviderTest, RejectsEmptyAudio) {
    const std::vector<float> empty;
    const auto audio = AudioInput::create(empty, 1000, 1).value();
    FakeTranscriptionProvider provider;

    const auto result = provider.transcribe(audio, options());
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(FakeTranscriptionProviderTest, RejectsNonFiniteSamples) {
    std::vector<float> samples(5000, 0.1f);
    samples[42] = std::numeric_limits<float>::quiet_NaN();
    const auto audio = AudioInput::create(samples, 1000, 1).value();
    FakeTranscriptionProvider provider;

    const auto result = provider.transcribe(audio, options());
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

}  // namespace
