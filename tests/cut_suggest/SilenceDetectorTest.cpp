#include "cut_suggest/SilenceDetector.h"

#include "audio_dsp/AudioBuffer.h"
#include "audio_dsp/AudioFormat.h"
#include "core/Timebase.h"
#include "cut_suggest/CutReason.h"
#include "cut_suggest/CutSuggestParameters.h"

#include "CutSuggestTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <limits>
#include <vector>

namespace {

using creator::audio_dsp::AudioBuffer;
using creator::audio_dsp::AudioFormat;
using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::cut_suggest::CutReason;
using creator::cut_suggest::CutSuggestParameters;
using creator::cut_suggest::SilenceDetector;
using cut_suggest_test::appendLevel;

constexpr std::uint32_t kRate = 48'000;
constexpr float kLoud = 0.3f;  // ~-10 dBFS, well above the -45 dBFS threshold

AudioFormat monoFormat() { return AudioFormat::create(kRate, 1).value(); }

SilenceDetector defaultDetector() {
    return SilenceDetector{CutSuggestParameters::create().value()};
}

// Frames for a whole number of milliseconds at the test sample rate.
std::size_t framesForMs(std::int64_t ms) {
    return static_cast<std::size_t>(kRate * ms / 1000);
}

std::int64_t nsForMs(std::int64_t ms) { return ms * 1'000'000; }

TEST(SilenceDetectorTest, FindsSingleGapBetweenTones) {
    std::vector<float> pcm;
    appendLevel(pcm, framesForMs(200), kLoud);   // tone
    appendLevel(pcm, framesForMs(600), 0.0f);    // >= 500 ms silence
    appendLevel(pcm, framesForMs(200), kLoud);   // tone
    const AudioFormat format = monoFormat();
    AudioBuffer buffer{pcm.data(), pcm.size(), format};

    const auto result = defaultDetector().detect(buffer);
    ASSERT_TRUE(result.hasValue());
    const auto& cuts = result.value();
    ASSERT_EQ(cuts.size(), 1u);
    EXPECT_EQ(cuts[0].reason(), CutReason::Silence);

    // The gap runs from 200 ms to 800 ms; boundaries are window-aligned so the
    // detected span should land within one 20 ms window of that.
    const DurationNs oneWindow =
        std::chrono::duration_cast<DurationNs>(std::chrono::milliseconds{20});
    const auto start = cuts[0].span().start().time_since_epoch();
    const auto end = cuts[0].span().end().time_since_epoch();
    EXPECT_NEAR(static_cast<double>(start.count()), static_cast<double>(nsForMs(200)),
                static_cast<double>(oneWindow.count()));
    EXPECT_NEAR(static_cast<double>(end.count()), static_cast<double>(nsForMs(800)),
                static_cast<double>(oneWindow.count()));
    EXPECT_GT(cuts[0].score(), 0.0);
    EXPECT_LE(cuts[0].score(), 1.0);
}

TEST(SilenceDetectorTest, ContinuouslyLoudYieldsNothing) {
    std::vector<float> pcm;
    appendLevel(pcm, framesForMs(1000), kLoud);
    const AudioFormat format = monoFormat();
    AudioBuffer buffer{pcm.data(), pcm.size(), format};

    const auto result = defaultDetector().detect(buffer);
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().empty());
}

TEST(SilenceDetectorTest, GapShorterThanMinimumYieldsNothing) {
    std::vector<float> pcm;
    appendLevel(pcm, framesForMs(200), kLoud);
    appendLevel(pcm, framesForMs(300), 0.0f);  // < 500 ms
    appendLevel(pcm, framesForMs(200), kLoud);
    const AudioFormat format = monoFormat();
    AudioBuffer buffer{pcm.data(), pcm.size(), format};

    const auto result = defaultDetector().detect(buffer);
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().empty());
}

TEST(SilenceDetectorTest, EmptyBufferYieldsNothing) {
    const AudioFormat format = monoFormat();
    AudioBuffer buffer{nullptr, 0, format};
    const auto result = defaultDetector().detect(buffer);
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().empty());
}

TEST(SilenceDetectorTest, RejectsNonFiniteSample) {
    std::vector<float> pcm;
    appendLevel(pcm, framesForMs(600), 0.0f);
    pcm[100] = std::numeric_limits<float>::quiet_NaN();
    const AudioFormat format = monoFormat();
    AudioBuffer buffer{pcm.data(), pcm.size(), format};

    const auto result = defaultDetector().detect(buffer);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(SilenceDetectorTest, IsDeterministic) {
    std::vector<float> pcm;
    appendLevel(pcm, framesForMs(200), kLoud);
    appendLevel(pcm, framesForMs(600), 0.0f);
    appendLevel(pcm, framesForMs(200), kLoud);
    const AudioFormat format = monoFormat();
    AudioBuffer buffer{pcm.data(), pcm.size(), format};

    const auto a = defaultDetector().detect(buffer);
    const auto b = defaultDetector().detect(buffer);
    ASSERT_TRUE(a.hasValue());
    ASSERT_TRUE(b.hasValue());
    EXPECT_EQ(a.value(), b.value());
}

}  // namespace
