#include "audio_dsp/GainProcessor.h"

#include "audio_dsp/AudioBuffer.h"
#include "audio_dsp/AudioFormat.h"
#include "audio_dsp/support/SyntheticAudio.h"
#include "core/AppError.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

namespace creator::audio_dsp {
namespace {

AudioFormat monoFormat() { return AudioFormat::create(48'000, 1).value(); }

// Constant-DC signal, handy for exact ratio and click assertions.
std::vector<float> dc(std::size_t frames, float value) {
    return std::vector<float>(frames, value);
}

TEST(GainProcessorTest, ZeroDbIsIdentity) {
    std::vector<float> storage = dc(64, 0.3F);
    const std::vector<float> original = storage;
    AudioBuffer buffer{storage.data(), storage.size(), monoFormat()};

    GainProcessor gain{0.0};
    ASSERT_TRUE(gain.process(buffer).hasValue());

    for (std::size_t i = 0; i < storage.size(); ++i) {
        EXPECT_FLOAT_EQ(storage[i], original[i]);
    }
}

TEST(GainProcessorTest, SixDbDoublesAmplitude) {
    std::vector<float> storage = dc(64, 0.25F);
    AudioBuffer buffer{storage.data(), storage.size(), monoFormat()};

    GainProcessor gain{6.020599913279624};  // 20*log10(2)
    ASSERT_TRUE(gain.process(buffer).hasValue());

    for (const float s : storage) {
        EXPECT_NEAR(s, 0.5F, 1e-4F);
    }
}

TEST(GainProcessorTest, NegativeInfinityDbProducesSilence) {
    std::vector<float> storage = dc(64, 0.9F);
    AudioBuffer buffer{storage.data(), storage.size(), monoFormat()};

    GainProcessor gain{-std::numeric_limits<double>::infinity()};
    ASSERT_TRUE(gain.process(buffer).hasValue());

    for (const float s : storage) {
        EXPECT_FLOAT_EQ(s, 0.0F);
    }
}

TEST(GainProcessorTest, VeryLowDbProducesEffectiveSilence) {
    std::vector<float> storage = dc(64, 0.9F);
    AudioBuffer buffer{storage.data(), storage.size(), monoFormat()};

    GainProcessor gain{-300.0};
    ASSERT_TRUE(gain.process(buffer).hasValue());

    for (const float s : storage) {
        EXPECT_NEAR(s, 0.0F, 1e-6F);
    }
}

TEST(GainProcessorTest, GainChangeRampsWithoutDiscontinuity) {
    GainProcessor gain{0.0};

    // First block establishes the applied factor at unity.
    std::vector<float> first = dc(512, 0.5F);
    AudioBuffer firstBuffer{first.data(), first.size(), monoFormat()};
    ASSERT_TRUE(gain.process(firstBuffer).hasValue());
    const float lastOfFirst = first.back();

    // Now jump the target by +12 dB and process a second block. The output
    // must not step by more than a small threshold between adjacent samples,
    // including across the buffer boundary.
    gain.setGainDb(12.0);
    std::vector<float> second = dc(512, 0.5F);
    AudioBuffer secondBuffer{second.data(), second.size(), monoFormat()};
    ASSERT_TRUE(gain.process(secondBuffer).hasValue());

    constexpr float kMaxStep = 0.02F;
    EXPECT_LT(std::abs(second.front() - lastOfFirst), kMaxStep);
    for (std::size_t i = 1; i < second.size(); ++i) {
        EXPECT_LT(std::abs(second[i] - second[i - 1]), kMaxStep)
            << "click at sample " << i;
    }
    // The ramp should have converged toward the +12 dB target (~3.981x) by the
    // end of a block longer than the 5 ms ramp window.
    EXPECT_NEAR(second.back(), 0.5F * std::pow(10.0F, 12.0F / 20.0F), 1e-3F);
}

TEST(GainProcessorTest, NonFiniteInputSampleRejected) {
    std::vector<float> storage = dc(64, 0.1F);
    storage[10] = std::numeric_limits<float>::quiet_NaN();
    AudioBuffer buffer{storage.data(), storage.size(), monoFormat()};

    GainProcessor gain{0.0};
    const auto result = gain.process(buffer);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), core::ErrorCode::InvalidArgument);
}

// reset() must discard an in-progress ramp so the next block is processed
// exactly as a freshly created instance at the current target gain would be.
TEST(GainProcessorTest, ResetDiscardsRampToFreshInstance) {
    GainProcessor hot{0.0};
    // Start a +12 dB ramp mid-flight, then reset before it settles.
    hot.setGainDb(12.0);
    std::vector<float> partial = dc(64, 0.5F);  // short block: ramp not done
    AudioBuffer partialBuf{partial.data(), partial.size(), monoFormat()};
    ASSERT_TRUE(hot.process(partialBuf).hasValue());
    hot.reset();

    // A fresh instance already sitting at +12 dB (current == target).
    GainProcessor fresh{12.0};

    std::vector<float> aStorage = dc(256, 0.5F);
    std::vector<float> bStorage = dc(256, 0.5F);
    AudioBuffer aBuf{aStorage.data(), aStorage.size(), monoFormat()};
    AudioBuffer bBuf{bStorage.data(), bStorage.size(), monoFormat()};
    ASSERT_TRUE(hot.process(aBuf).hasValue());
    ASSERT_TRUE(fresh.process(bBuf).hasValue());

    for (std::size_t i = 0; i < aStorage.size(); ++i) {
        EXPECT_EQ(aStorage[i], bStorage[i]) << "at " << i;
    }
}

TEST(GainProcessorTest, EmptyBufferIsNoOp) {
    AudioBuffer buffer{nullptr, 0, monoFormat()};
    GainProcessor gain{6.0};
    EXPECT_TRUE(gain.process(buffer).hasValue());
}

}  // namespace
}  // namespace creator::audio_dsp
