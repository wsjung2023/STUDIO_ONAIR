#include "capture/AudioLevelMeter.h"
#include "capture/BoundedAudioBlockQueue.h"

#include "core/Timebase.h"
#include "media/MediaTypes.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace {

using creator::capture::AudioLevelMeter;
using creator::capture::BoundedAudioBlockQueue;
using creator::media::AudioBlock;

AudioBlock blockAt(std::int64_t timestamp, std::vector<float> samples,
                   std::uint32_t channels = 1) {
    auto owned = std::shared_ptr<float[]>(new float[samples.size()],
                                         std::default_delete<float[]>{});
    for (std::size_t index = 0; index < samples.size(); ++index) {
        owned[index] = samples[index];
    }
    AudioBlock block;
    block.timestamp = creator::core::TimestampNs{creator::core::Nanoseconds{timestamp}};
    block.sampleRate = 48000;
    block.channels = channels;
    block.frameCount = static_cast<std::uint32_t>(samples.size() / channels);
    block.samples = std::move(owned);
    return block;
}

TEST(BoundedAudioBlockQueueTest, PreservesFifoOrderAndMetadata) {
    BoundedAudioBlockQueue queue{3};

    ASSERT_TRUE(queue.tryPush(blockAt(10, {0.1F})).hasValue());
    ASSERT_TRUE(queue.tryPush(blockAt(20, {0.2F})).hasValue());

    auto first = queue.tryPop();
    auto second = queue.tryPop();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->timestamp.time_since_epoch().count(), 10);
    EXPECT_EQ(second->timestamp.time_since_epoch().count(), 20);
    EXPECT_FALSE(queue.tryPop().has_value());
}

TEST(BoundedAudioBlockQueueTest, RejectsOverflowAndCountsEveryRejectedBlock) {
    BoundedAudioBlockQueue queue{1};
    ASSERT_TRUE(queue.tryPush(blockAt(1, {0.1F})).hasValue());

    const auto second = queue.tryPush(blockAt(2, {0.2F}));
    const auto third = queue.tryPush(blockAt(3, {0.3F}));

    ASSERT_FALSE(second.hasValue());
    ASSERT_FALSE(third.hasValue());
    EXPECT_EQ(second.error().code(), creator::core::ErrorCode::InvalidState);
    EXPECT_EQ(queue.overruns(), 2u);
    EXPECT_EQ(queue.size(), 1u);
}

TEST(BoundedAudioBlockQueueTest, ClearReleasesOwnedSamplesAndResetsSizeOnly) {
    BoundedAudioBlockQueue queue{2};
    auto samples = std::shared_ptr<float[]>(new float[1], std::default_delete<float[]>{});
    std::weak_ptr<const float[]> lifetime = samples;
    AudioBlock block;
    block.frameCount = 1;
    block.channels = 1;
    block.samples = std::move(samples);
    ASSERT_TRUE(queue.tryPush(std::move(block)).hasValue());
    ASSERT_FALSE(lifetime.expired());

    queue.clear();

    EXPECT_TRUE(lifetime.expired());
    EXPECT_EQ(queue.size(), 0u);
    EXPECT_EQ(queue.capacity(), 2u);
}

TEST(BoundedAudioBlockQueueTest, SupportsOneProducerAndOneConsumerWithoutLoss) {
    constexpr std::int64_t count = 2000;
    BoundedAudioBlockQueue queue{static_cast<std::size_t>(count)};
    std::atomic<bool> start{false};
    std::atomic<std::int64_t> consumed{0};
    std::atomic<bool> orderCorrect{true};

    std::thread producer([&] {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (std::int64_t value = 0; value < count; ++value) {
            if (!queue.tryPush(blockAt(value, {0.0F})).hasValue()) {
                orderCorrect.store(false, std::memory_order_relaxed);
            }
        }
    });
    std::thread consumer([&] {
        while (!start.load(std::memory_order_acquire)) {
        }
        std::int64_t expected = 0;
        while (expected < count) {
            auto block = queue.tryPop();
            if (!block.has_value()) {
                std::this_thread::yield();
                continue;
            }
            if (block->timestamp.time_since_epoch().count() != expected) {
                orderCorrect.store(false, std::memory_order_relaxed);
            }
            ++expected;
        }
        consumed.store(expected, std::memory_order_relaxed);
    });

    start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    EXPECT_TRUE(orderCorrect.load());
    EXPECT_EQ(consumed.load(), count);
    EXPECT_EQ(queue.overruns(), 0u);
}

TEST(AudioLevelMeterTest, CalculatesPeakRmsAndDbfsAcrossChannels) {
    const auto block = blockAt(0, {1.0F, -1.0F, 0.0F, 0.0F}, 2);

    const auto result = AudioLevelMeter::measure(block);

    ASSERT_TRUE(result.hasValue());
    EXPECT_DOUBLE_EQ(result.value().peakLinear, 1.0);
    EXPECT_NEAR(result.value().rmsLinear, std::sqrt(0.5), 1e-7);
    EXPECT_DOUBLE_EQ(result.value().peakDbfs, 0.0);
    EXPECT_NEAR(result.value().rmsDbfs, -3.0102999566, 1e-7);
}

TEST(AudioLevelMeterTest, FloorsDigitalSilenceAtMinus96Dbfs) {
    const auto result = AudioLevelMeter::measure(blockAt(0, {0.0F, 0.0F}));

    ASSERT_TRUE(result.hasValue());
    EXPECT_DOUBLE_EQ(result.value().peakLinear, 0.0);
    EXPECT_DOUBLE_EQ(result.value().rmsLinear, 0.0);
    EXPECT_DOUBLE_EQ(result.value().peakDbfs, -96.0);
    EXPECT_DOUBLE_EQ(result.value().rmsDbfs, -96.0);
}

TEST(AudioLevelMeterTest, RejectsMissingSamplesOrInconsistentMetadata) {
    AudioBlock missing;
    missing.frameCount = 1;
    missing.channels = 1;
    AudioBlock noFrames = blockAt(0, {0.0F});
    noFrames.frameCount = 0;
    AudioBlock noChannels = blockAt(0, {0.0F});
    noChannels.channels = 0;

    EXPECT_FALSE(AudioLevelMeter::measure(missing).hasValue());
    EXPECT_FALSE(AudioLevelMeter::measure(noFrames).hasValue());
    EXPECT_FALSE(AudioLevelMeter::measure(noChannels).hasValue());
}

TEST(AudioLevelMeterTest, RejectsNonFiniteSamples) {
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto infinity = std::numeric_limits<float>::infinity();

    EXPECT_FALSE(AudioLevelMeter::measure(blockAt(0, {nan})).hasValue());
    EXPECT_FALSE(AudioLevelMeter::measure(blockAt(0, {infinity})).hasValue());
}

}  // namespace
