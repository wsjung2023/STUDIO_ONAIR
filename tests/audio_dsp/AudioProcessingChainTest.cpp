#include "audio_dsp/AudioProcessingChain.h"

#include "audio_dsp/AudioBuffer.h"
#include "audio_dsp/AudioFormat.h"
#include "audio_dsp/CompressorProcessor.h"
#include "audio_dsp/GainProcessor.h"
#include "audio_dsp/LimiterProcessor.h"
#include "audio_dsp/support/SyntheticAudio.h"
#include "core/AppError.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

namespace creator::audio_dsp {
namespace {

using std::chrono::microseconds;
using std::chrono::milliseconds;

AudioFormat monoFormat() { return AudioFormat::create(48'000, 1).value(); }

constexpr std::size_t secToFrames(double seconds) {
    return static_cast<std::size_t>(seconds * 48'000.0);
}

double maxAbs(const std::vector<float>& samples, std::size_t start,
              std::size_t end) {
    double peak = 0.0;
    for (std::size_t i = start; i < end; ++i) {
        peak = std::max(peak, std::abs(static_cast<double>(samples[i])));
    }
    return peak;
}

std::unique_ptr<IAudioProcessor> makeGain(double gainDb) {
    return std::make_unique<GainProcessor>(gainDb);
}

std::unique_ptr<IAudioProcessor> makeLimiter() {
    LimiterProcessor::Parameters p;
    p.ceilingDbtp = -1.0;
    p.lookAhead = microseconds{1500};  // 72 frames @ 48 kHz
    p.release = milliseconds{100};
    auto lim = LimiterProcessor::create(p, monoFormat());
    EXPECT_TRUE(lim.hasValue());
    return std::make_unique<LimiterProcessor>(std::move(lim).value());
}

std::unique_ptr<IAudioProcessor> makeCompressor() {
    CompressorProcessor::Parameters p;  // zero-latency
    auto comp = CompressorProcessor::create(p);
    EXPECT_TRUE(comp.hasValue());
    return std::make_unique<CompressorProcessor>(std::move(comp).value());
}

// Test-only node that returns an error unconditionally, to prove the chain
// stops at the first failure.
class FailingProcessor final : public IAudioProcessor {
public:
    [[nodiscard]] core::Result<void> process(AudioBuffer& /*buffer*/) override {
        return core::AppError{core::ErrorCode::InvalidState, "boom"};
    }
};

// Test-only node that records how many times it was invoked.
class CountingProcessor final : public IAudioProcessor {
public:
    explicit CountingProcessor(int* counter) : counter_(counter) {}
    [[nodiscard]] core::Result<void> process(AudioBuffer& /*buffer*/) override {
        ++*counter_;
        return core::ok();
    }

private:
    int* counter_;
};

TEST(AudioProcessingChainTest, EmptyChainIsIdentity) {
    AudioProcessingChain chain;
    EXPECT_TRUE(chain.empty());
    EXPECT_EQ(chain.size(), 0u);

    auto sig = testing::makeSine(monoFormat(), secToFrames(0.05), 1000.0, -6.0);
    const std::vector<float> original = sig.samples();

    AudioBuffer buf = sig.view();
    ASSERT_TRUE(chain.process(buf).hasValue());
    EXPECT_EQ(sig.samples(), original);  // untouched
}

TEST(AudioProcessingChainTest, OrderMattersGainThenLimiterVsLimiterThenGain) {
    const std::size_t frames = secToFrames(0.3);
    const double boostDb = 12.0;

    // [Gain +12] -> [Limiter -1 dBTP]: the limiter is LAST, so it clamps the
    // boosted signal to the ceiling.
    AudioProcessingChain gainThenLimit;
    gainThenLimit.add(makeGain(boostDb)).add(makeLimiter());

    // [Limiter -1 dBTP] -> [Gain +12]: the gain is LAST, so it re-inflates the
    // limited signal far past the ceiling.
    AudioProcessingChain limitThenGain;
    limitThenGain.add(makeLimiter()).add(makeGain(boostDb));

    auto sigA = testing::makeSine(monoFormat(), frames, 1000.0, 0.0);  // hot
    auto sigB = testing::makeSine(monoFormat(), frames, 1000.0, 0.0);
    AudioBuffer bufA = sigA.view();
    AudioBuffer bufB = sigB.view();
    ASSERT_TRUE(gainThenLimit.process(bufA).hasValue());
    ASSERT_TRUE(limitThenGain.process(bufB).hasValue());

    // Measure the steady interior (past look-ahead priming and ramp).
    const std::size_t start = secToFrames(0.15);
    const double ceilingLinear = std::pow(10.0, -1.0 / 20.0);  // -1 dBFS
    const double peakA = maxAbs(sigA.samples(), start, frames);
    const double peakB = maxAbs(sigB.samples(), start, frames);

    EXPECT_LE(peakA, ceilingLinear + 1e-3);  // limiter last -> clamped
    EXPECT_GT(peakB, ceilingLinear * 2.0);   // gain last -> way over ceiling
    EXPECT_GT(std::abs(peakA - peakB), 0.5);  // order genuinely changes output
}

TEST(AudioProcessingChainTest, FirstNodeErrorStopsChainAndPropagates) {
    int downstreamCalls = 0;
    AudioProcessingChain chain;
    chain.add(std::make_unique<FailingProcessor>())
        .add(std::make_unique<CountingProcessor>(&downstreamCalls));

    auto sig = testing::makeSine(monoFormat(), secToFrames(0.02), 1000.0, -6.0);
    AudioBuffer buf = sig.view();
    const auto r = chain.process(buf);

    ASSERT_FALSE(r.hasValue());
    EXPECT_EQ(r.error().code(), core::ErrorCode::InvalidState);
    EXPECT_EQ(downstreamCalls, 0);  // downstream node never ran
}

TEST(AudioProcessingChainTest, AllNodesRunInOrderOnSuccess) {
    int callsA = 0;
    int callsB = 0;
    AudioProcessingChain chain;
    chain.add(std::make_unique<CountingProcessor>(&callsA))
        .add(std::make_unique<CountingProcessor>(&callsB));

    auto sig = testing::makeSine(monoFormat(), secToFrames(0.02), 1000.0, -6.0);
    AudioBuffer buf = sig.view();
    ASSERT_TRUE(chain.process(buf).hasValue());
    EXPECT_EQ(callsA, 1);
    EXPECT_EQ(callsB, 1);
}

TEST(AudioProcessingChainTest, LatencyFramesIsSumOfNodeLatencies) {
    AudioProcessingChain empty;
    EXPECT_EQ(empty.latencyFrames(), 0u);

    // Compressor (0) + Limiter (72) == 72.
    AudioProcessingChain chain;
    chain.add(makeCompressor()).add(makeLimiter());
    EXPECT_EQ(chain.latencyFrames(), 72u);

    // Two limiters: 72 + 72 == 144.
    AudioProcessingChain twoLimiters;
    twoLimiters.add(makeLimiter()).add(makeLimiter());
    EXPECT_EQ(twoLimiters.latencyFrames(), 144u);
}

TEST(AudioProcessingChainTest, NullNodeIgnored) {
    AudioProcessingChain chain;
    chain.add(nullptr);
    EXPECT_TRUE(chain.empty());
    EXPECT_EQ(chain.latencyFrames(), 0u);
}

}  // namespace
}  // namespace creator::audio_dsp
