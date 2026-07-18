#include "audio_dsp/AudioBuffer.h"

#include "audio_dsp/AudioFormat.h"

#include <gtest/gtest.h>

#include <vector>

namespace creator::audio_dsp {
namespace {

AudioFormat stereo() { return AudioFormat::create(48'000, 2).value(); }

TEST(AudioBufferTest, ReportsGeometryFromFormatAndFrames) {
    std::vector<float> storage(8, 0.0F);  // 4 frames * 2 channels
    AudioBuffer buffer{storage.data(), 4, stereo()};
    EXPECT_EQ(buffer.frameCount(), 4u);
    EXPECT_EQ(buffer.channelCount(), 2u);
    EXPECT_EQ(buffer.sampleCount(), 8u);
    EXPECT_FALSE(buffer.empty());
}

TEST(AudioBufferTest, SampleAccessIsFrameMajorAndMutable) {
    std::vector<float> storage(6, 0.0F);  // 3 frames * 2 channels
    AudioBuffer buffer{storage.data(), 3, stereo()};
    buffer.sample(1, 0) = 0.5F;
    buffer.sample(1, 1) = -0.5F;
    // Interleaved: frame 1 occupies indices 2 and 3.
    EXPECT_FLOAT_EQ(storage[2], 0.5F);
    EXPECT_FLOAT_EQ(storage[3], -0.5F);
    const AudioBuffer& constView = buffer;
    EXPECT_FLOAT_EQ(constView.sample(1, 0), 0.5F);
}

TEST(AudioBufferTest, NullPointerYieldsEmptyView) {
    AudioBuffer buffer{nullptr, 128, stereo()};
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.frameCount(), 0u);
    EXPECT_EQ(buffer.sampleCount(), 0u);
    EXPECT_EQ(buffer.data(), nullptr);
}

TEST(AudioBufferTest, ZeroFramesIsEmpty) {
    std::vector<float> storage(2, 0.0F);
    AudioBuffer buffer{storage.data(), 0, stereo()};
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.sampleCount(), 0u);
}

}  // namespace
}  // namespace creator::audio_dsp
