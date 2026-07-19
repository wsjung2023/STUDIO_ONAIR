#include "rnnoise_adapter/RnnoiseDenoiseProcessor.h"

#include "audio_dsp/AudioBuffer.h"
#include "audio_dsp/AudioFormat.h"
#include "core/AppError.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

// CS_TEST_RNNOISE_ROOT is the audited prefix staged by bootstrap_rnnoise.ps1;
// this whole file only builds when CS_ENABLE_RNNOISE is ON.
#ifndef CS_TEST_RNNOISE_ROOT
#error "CS_TEST_RNNOISE_ROOT must be defined for the RNNoise-enabled tests"
#endif

namespace {

using creator::audio_dsp::AudioBuffer;
using creator::audio_dsp::AudioFormat;
using creator::audio_dsp::IAudioProcessor;
using creator::core::ErrorCode;
using creator::rnnoise_adapter::createRnnoiseDenoiseProcessor;
using creator::rnnoise_adapter::RnnoiseDenoiseProcessor;

std::unique_ptr<IAudioProcessor> makeProcessor() {
    auto result = createRnnoiseDenoiseProcessor(CS_TEST_RNNOISE_ROOT);
    EXPECT_TRUE(result.hasValue())
        << (result.hasValue() ? "" : result.error().message());
    return result.hasValue() ? std::move(result).value() : nullptr;
}

// Deterministic pseudo-random noise (LCG) — no clock, no time-seeded RNG, so
// the test is reproducible (CLAUDE.md §5/§8).
std::vector<float> deterministicNoise(std::size_t sampleCount, float amplitude) {
    std::vector<float> out(sampleCount);
    std::uint32_t state = 0x1234567U;
    for (std::size_t i = 0; i < sampleCount; ++i) {
        state = state * 1664525U + 1013904223U;
        const float unit =
            (static_cast<float>(state >> 8) / static_cast<float>(1U << 24)) *
                2.0F -
            1.0F;
        out[i] = unit * amplitude;
    }
    return out;
}

double rms(const std::vector<float>& data, std::size_t begin, std::size_t end) {
    double sum = 0.0;
    for (std::size_t i = begin; i < end; ++i) {
        sum += static_cast<double>(data[i]) * static_cast<double>(data[i]);
    }
    const std::size_t n = end - begin;
    return n == 0 ? 0.0 : std::sqrt(sum / static_cast<double>(n));
}

TEST(RnnoiseDenoiseProcessorTest, CreatesAgainstAuditedRuntime) {
    auto processor = makeProcessor();
    ASSERT_NE(processor, nullptr);
    EXPECT_EQ(processor->latencyFrames(), RnnoiseDenoiseProcessor::kFrameSize);
}

TEST(RnnoiseDenoiseProcessorTest, RejectsNon48kHz) {
    auto processor = makeProcessor();
    ASSERT_NE(processor, nullptr);
    const AudioFormat wrong = AudioFormat::create(44100, 1).value();
    std::vector<float> samples(480, 0.1F);
    AudioBuffer buffer{samples.data(), 480, wrong};
    const auto result = processor->process(buffer);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(RnnoiseDenoiseProcessorTest, RejectsNonFiniteInput) {
    auto processor = makeProcessor();
    ASSERT_NE(processor, nullptr);
    const AudioFormat mono = AudioFormat::create(48000, 1).value();
    std::vector<float> samples(480, 0.1F);
    samples[100] = std::numeric_limits<float>::quiet_NaN();
    AudioBuffer buffer{samples.data(), 480, mono};
    const auto result = processor->process(buffer);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

// Feed ~1 s of pure noise (no speech). A noise suppressor should attenuate it:
// after the one-frame latency + settling the output energy must be clearly
// below the input energy, and every output sample stays finite and bounded.
TEST(RnnoiseDenoiseProcessorTest, SuppressesPureNoiseEnergy) {
    auto processor = makeProcessor();
    ASSERT_NE(processor, nullptr);
    const AudioFormat mono = AudioFormat::create(48000, 1).value();

    constexpr std::size_t kFrame = RnnoiseDenoiseProcessor::kFrameSize;
    constexpr std::size_t kBlocks = 100;  // 100 * 480 = 48000 samples ~ 1 s
    const std::vector<float> input = deterministicNoise(kFrame * kBlocks, 0.3F);
    std::vector<float> work = input;

    for (std::size_t block = 0; block < kBlocks; ++block) {
        AudioBuffer buffer{work.data() + block * kFrame, kFrame, mono};
        ASSERT_TRUE(processor->process(buffer).hasValue());
    }

    for (const float sample : work) {
        ASSERT_TRUE(std::isfinite(sample));
        ASSERT_LE(std::fabs(sample), 1.0F);
    }

    // Measure over a settled tail, aligned for the one-frame output latency.
    const std::size_t tailBegin = kFrame * (kBlocks / 2);
    const std::size_t tailEnd = kFrame * kBlocks;
    const double inputRms = rms(input, tailBegin - kFrame, tailEnd - kFrame);
    const double outputRms = rms(work, tailBegin, tailEnd);

    EXPECT_GT(inputRms, 0.0);
    EXPECT_LT(outputRms, inputRms * 0.9)
        << "output RMS " << outputRms << " not suppressed below input RMS "
        << inputRms;
}

TEST(RnnoiseDenoiseProcessorTest, ResetRePrimesLatency) {
    auto processor = makeProcessor();
    ASSERT_NE(processor, nullptr);
    const AudioFormat mono = AudioFormat::create(48000, 1).value();

    std::vector<float> samples =
        deterministicNoise(RnnoiseDenoiseProcessor::kFrameSize, 0.2F);
    AudioBuffer buffer{samples.data(), RnnoiseDenoiseProcessor::kFrameSize, mono};
    ASSERT_TRUE(processor->process(buffer).hasValue());

    auto* real = dynamic_cast<RnnoiseDenoiseProcessor*>(processor.get());
    ASSERT_NE(real, nullptr);
    real->reset();

    // After reset the first emitted frame is the silence priming again.
    std::vector<float> next =
        deterministicNoise(RnnoiseDenoiseProcessor::kFrameSize, 0.2F);
    AudioBuffer nextBuffer{next.data(), RnnoiseDenoiseProcessor::kFrameSize, mono};
    ASSERT_TRUE(processor->process(nextBuffer).hasValue());
    for (const float sample : next) {
        EXPECT_FLOAT_EQ(sample, 0.0F);
    }
}

// Stereo: each channel is denoised by its own DenoiseState. Both channels are
// processed and stay finite/bounded.
TEST(RnnoiseDenoiseProcessorTest, HandlesMultiChannelIndependently) {
    auto processor = makeProcessor();
    ASSERT_NE(processor, nullptr);
    const AudioFormat stereo = AudioFormat::create(48000, 2).value();

    constexpr std::size_t kFrame = RnnoiseDenoiseProcessor::kFrameSize;
    std::vector<float> interleaved(kFrame * 2 * 4);
    const auto noise = deterministicNoise(kFrame * 4, 0.25F);
    for (std::size_t i = 0; i < kFrame * 4; ++i) {
        interleaved[i * 2] = noise[i];
        interleaved[i * 2 + 1] = noise[i] * 0.5F;
    }
    for (std::size_t block = 0; block < 4; ++block) {
        AudioBuffer buffer{interleaved.data() + block * kFrame * 2, kFrame,
                           stereo};
        ASSERT_TRUE(processor->process(buffer).hasValue());
    }
    for (const float sample : interleaved) {
        ASSERT_TRUE(std::isfinite(sample));
        ASSERT_LE(std::fabs(sample), 1.0F);
    }
}

}  // namespace
