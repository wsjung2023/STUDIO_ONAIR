#include "audio_dsp/DenoiseProcessorFactory.h"
#include "audio_dsp/UnavailableDenoiseProcessor.h"

#include "audio_dsp/AudioBuffer.h"
#include "audio_dsp/AudioFormat.h"
#include "audio_dsp/AudioProcessingChain.h"
#include "core/AppError.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace {

using creator::audio_dsp::AudioBuffer;
using creator::audio_dsp::AudioFormat;
using creator::audio_dsp::AudioProcessingChain;
using creator::audio_dsp::IAudioProcessor;
using creator::audio_dsp::UnavailableDenoiseProcessor;
using creator::audio_dsp::makeUnavailableDenoiseProcessor;
using creator::core::ErrorCode;

AudioFormat stereo48k() { return AudioFormat::create(48000, 2).value(); }

// The default build (CS_ENABLE_RNNOISE=OFF) selects the Unavailable denoise
// node for the reserved ML-denoise slot. It must refuse to process audio with a
// clear, recoverable error rather than silently pretend denoise happened.
TEST(UnavailableDenoiseProcessorTest, RejectsNonEmptyBufferWithClearError) {
    UnavailableDenoiseProcessor processor;
    std::vector<float> samples(2 * 480, 0.25F);
    AudioBuffer buffer{samples.data(), 480, stereo48k()};

    const auto result = processor.process(buffer);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
    EXPECT_NE(result.error().message().find("not built"), std::string::npos);
}

// An empty buffer is a valid no-op input per the IAudioProcessor contract, so
// even the Unavailable node accepts it (nothing to deny) and adds no latency.
TEST(UnavailableDenoiseProcessorTest, AcceptsEmptyBufferAndReportsNoLatency) {
    UnavailableDenoiseProcessor processor;
    AudioBuffer empty{nullptr, 0, stereo48k()};

    EXPECT_TRUE(processor.process(empty).hasValue());
    EXPECT_EQ(processor.latencyFrames(), 0U);
}

// The OFF-path factory yields exactly that node.
TEST(UnavailableDenoiseProcessorTest, FactoryReturnsUnavailableNode) {
    std::unique_ptr<IAudioProcessor> node = makeUnavailableDenoiseProcessor();
    ASSERT_NE(node, nullptr);

    std::vector<float> samples(2 * 480, 0.5F);
    AudioBuffer buffer{samples.data(), 480, stereo48k()};
    const auto result = node->process(buffer);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
}

// Placed at the front of the chain (the reserved index-0 slot), the Unavailable
// node makes the whole chain fail fast — a downstream compressor/limiter never
// runs on audio a missing denoiser could not clean (CLAUDE.md §5).
TEST(UnavailableDenoiseProcessorTest, ChainStopsAtUnavailableDenoiseSlot) {
    AudioProcessingChain chain;
    chain.add(makeUnavailableDenoiseProcessor());
    EXPECT_EQ(chain.size(), 1U);

    std::vector<float> samples(2 * 480, 0.1F);
    AudioBuffer buffer{samples.data(), 480, stereo48k()};
    EXPECT_FALSE(chain.process(buffer).hasValue());
}

}  // namespace
