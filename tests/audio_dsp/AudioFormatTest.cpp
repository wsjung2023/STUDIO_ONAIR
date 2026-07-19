#include "audio_dsp/AudioFormat.h"

#include "core/AppError.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

namespace creator::audio_dsp {
namespace {

TEST(AudioFormatTest, ValidFormatExposesRateAndChannels) {
    const auto format = AudioFormat::create(48'000, 2);
    ASSERT_TRUE(format.hasValue());
    EXPECT_EQ(format.value().sampleRateHz(), 48'000u);
    EXPECT_EQ(format.value().channelCount(), 2u);
}

TEST(AudioFormatTest, ZeroSampleRateRejected) {
    const auto format = AudioFormat::create(0, 2);
    ASSERT_FALSE(format.hasValue());
    EXPECT_EQ(format.error().code(), core::ErrorCode::InvalidArgument);
}

TEST(AudioFormatTest, ZeroChannelsRejected) {
    const auto format = AudioFormat::create(48'000, 0);
    ASSERT_FALSE(format.hasValue());
    EXPECT_EQ(format.error().code(), core::ErrorCode::InvalidArgument);
}

TEST(AudioFormatTest, TooManyChannelsRejected) {
    const auto format = AudioFormat::create(48'000, AudioFormat::kMaxChannels + 1);
    ASSERT_FALSE(format.hasValue());
    EXPECT_EQ(format.error().code(), core::ErrorCode::InvalidArgument);
}

TEST(AudioFormatTest, MaxChannelsAccepted) {
    EXPECT_TRUE(AudioFormat::create(48'000, AudioFormat::kMaxChannels).hasValue());
}

TEST(AudioFormatTest, DurationForFramesIsTyped) {
    const auto format = AudioFormat::create(48'000, 2).value();
    EXPECT_EQ(format.durationForFrames(48'000),
              std::chrono::nanoseconds{std::chrono::seconds{1}});
    EXPECT_EQ(format.durationForFrames(24'000),
              std::chrono::nanoseconds{std::chrono::milliseconds{500}});
}

// A very large frame count (200 hours @ 48 kHz = 3.456e10 frames) would overflow
// u64 in the naive `frames * 1e9` product (>1.84e19). The split computation must
// stay exact and return the correct, sane duration.
TEST(AudioFormatTest, DurationForVeryLargeFrameCountDoesNotOverflow) {
    const auto format = AudioFormat::create(48'000, 2).value();
    // 200 hours worth of frames.
    const std::uint64_t frames = 200ULL * 3600ULL * 48'000ULL;  // 34,560,000,000
    EXPECT_EQ(format.durationForFrames(frames),
              std::chrono::nanoseconds{std::chrono::hours{200}});

    // A non-multiple of the sample rate stays exact too (1 s + 1 frame).
    EXPECT_EQ(format.durationForFrames(48'001),
              std::chrono::nanoseconds{std::chrono::seconds{1}} +
                  std::chrono::nanoseconds{20'833});  // 1e9/48000 truncated
}

TEST(AudioFormatTest, EqualityComparesRateAndChannels) {
    const auto a = AudioFormat::create(48'000, 2).value();
    const auto b = AudioFormat::create(48'000, 2).value();
    const auto c = AudioFormat::create(44'100, 2).value();
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

}  // namespace
}  // namespace creator::audio_dsp
