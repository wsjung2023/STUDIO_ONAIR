#include "audio_dsp/AudioCleanupChain.h"

#include "audio_dsp/AudioBuffer.h"
#include "audio_dsp/AudioFormat.h"
#include "audio_dsp/IAudioProcessor.h"
#include "audio_dsp/LimiterProcessor.h"
#include "audio_dsp/LoudnessMeter.h"
#include "audio_dsp/support/SyntheticAudio.h"
#include "core/AppError.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace creator::audio_dsp {
namespace {

AudioFormat stereoFormat() { return AudioFormat::create(48'000, 2).value(); }

constexpr std::size_t secToFrames(double seconds) {
    return static_cast<std::size_t>(seconds * 48'000.0);
}

/// A denoise-slot stand-in that always fails on process(): because the chain
/// stops at the FIRST error, seeing THIS error proves the node ran at index 0
/// (ahead of the compressor and limiter).
class FailingProbe final : public IAudioProcessor {
public:
    [[nodiscard]] core::Result<void> process(AudioBuffer&) override {
        return core::AppError{core::ErrorCode::InvalidState, "probe ran first"};
    }
};

TEST(AudioCleanupChainTest, DefaultChainIsCompressorThenLimiter) {
    auto chain = makeAudioCleanupChain(stereoFormat());
    ASSERT_TRUE(chain.hasValue());
    ASSERT_NE(chain.value(), nullptr);
    // No denoise in the default build: compressor + limiter only.
    EXPECT_EQ(chain.value()->size(), 2U);

    // The only latency in the chain is the limiter's look-ahead, so the chain
    // reports exactly that for A/V-sync compensation (CLAUDE.md §5).
    auto limiter = LimiterProcessor::create(LimiterProcessor::Parameters{},
                                            stereoFormat());
    ASSERT_TRUE(limiter.hasValue());
    EXPECT_EQ(chain.value()->latencyFrames(), limiter.value().latencyFrames());
}

TEST(AudioCleanupChainTest, NullDenoiseIsOmitted) {
    auto chain = makeAudioCleanupChain(stereoFormat(), nullptr);
    ASSERT_TRUE(chain.hasValue());
    EXPECT_EQ(chain.value()->size(), 2U);
}

TEST(AudioCleanupChainTest, PrependsDenoiseAtFront) {
    auto chain = makeAudioCleanupChain(stereoFormat(),
                                       std::make_unique<FailingProbe>());
    ASSERT_TRUE(chain.hasValue());
    EXPECT_EQ(chain.value()->size(), 3U);  // denoise + compressor + limiter

    // Process a block: the failing denoise at index 0 stops the chain first.
    auto sig = testing::makeSine(stereoFormat(), secToFrames(0.1), 1000.0, -12.0);
    AudioBuffer view = sig.view();
    auto r = chain.value()->process(view);
    ASSERT_FALSE(r.hasValue());
    EXPECT_EQ(r.error().code(), core::ErrorCode::InvalidState);  // the probe's error
}

TEST(AudioCleanupChainTest, LimiterHoldsTruePeakUnderCeiling) {
    AudioCleanupParameters params;
    params.limiter.ceilingDbtp = -1.0;
    auto chain = makeAudioCleanupChain(stereoFormat(), nullptr, params);
    ASSERT_TRUE(chain.hasValue());

    // A hot 0 dBFS sine would blow past -1 dBTP without limiting.
    auto sig = testing::makeSine(stereoFormat(), secToFrames(1.0), 1000.0, 0.0);
    AudioBuffer view = sig.view();
    ASSERT_TRUE(chain.value()->process(view).hasValue());

    auto meter = LoudnessMeter::create(stereoFormat());
    ASSERT_TRUE(meter.hasValue());
    AudioBuffer measureView = sig.view();
    ASSERT_TRUE(meter.value().addBlock(measureView).hasValue());
    // 4x oversampled true peak stays at/under the ceiling (small tolerance for
    // the detector's documented ~0.7 dB underestimate is not needed here since
    // we assert against the ceiling the limiter targets).
    EXPECT_LE(meter.value().truePeakDbtp(), -1.0 + 0.15);
}

TEST(AudioCleanupChainTest, PropagatesInvalidCompressorParameters) {
    AudioCleanupParameters params;
    params.compressor.ratio = 0.5;  // < 1 is rejected by CompressorProcessor
    auto chain = makeAudioCleanupChain(stereoFormat(), nullptr, params);
    ASSERT_FALSE(chain.hasValue());
    EXPECT_EQ(chain.error().code(), core::ErrorCode::InvalidArgument);
}

TEST(AudioCleanupChainTest, PropagatesInvalidLimiterParameters) {
    AudioCleanupParameters params;
    params.limiter.ceilingDbtp = 3.0;  // above full scale is rejected
    auto chain = makeAudioCleanupChain(stereoFormat(), nullptr, params);
    ASSERT_FALSE(chain.hasValue());
    EXPECT_EQ(chain.error().code(), core::ErrorCode::InvalidArgument);
}

}  // namespace
}  // namespace creator::audio_dsp
