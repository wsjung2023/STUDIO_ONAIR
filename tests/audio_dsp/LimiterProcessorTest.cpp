#include "audio_dsp/LimiterProcessor.h"

#include "audio_dsp/AudioBuffer.h"
#include "audio_dsp/AudioFormat.h"
#include "audio_dsp/LoudnessMeter.h"
#include "audio_dsp/support/SyntheticAudio.h"
#include "core/AppError.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <numbers>
#include <vector>

namespace creator::audio_dsp {
namespace {

using std::chrono::microseconds;
using std::chrono::milliseconds;

AudioFormat monoFormat() { return AudioFormat::create(48'000, 1).value(); }
AudioFormat stereoFormat() { return AudioFormat::create(48'000, 2).value(); }

constexpr std::size_t secToFrames(double seconds) {
    return static_cast<std::size_t>(seconds * 48'000.0);
}

// fs/4 sine with a pi/4 phase: sample peak = A/sqrt2 but the true (inter-sample)
// peak is A — the crest lands squarely between samples, exactly what a true-peak
// limiter must catch. Writes the same waveform to every channel.
testing::AudioSignal makeInterSampleSine(const AudioFormat& fmt,
                                         std::size_t frames, double amplitude) {
    testing::AudioSignal sig{fmt, frames};
    const std::uint32_t channels = fmt.channelCount();
    for (std::size_t n = 0; n < frames; ++n) {
        const double phase = std::numbers::pi / 2.0 * static_cast<double>(n) +
                             std::numbers::pi / 4.0;
        const float v = static_cast<float>(amplitude * std::sin(phase));
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            sig.samples()[n * channels + ch] = v;
        }
    }
    return sig;
}

double measureTruePeakDbtp(const testing::AudioSignal& sig) {
    auto meter = LoudnessMeter::create(monoFormat());
    EXPECT_TRUE(meter.hasValue());
    AudioBuffer view = const_cast<testing::AudioSignal&>(sig).view();
    EXPECT_TRUE(meter.value().addBlock(view).hasValue());
    return meter.value().truePeakDbtp();
}

double maxAbsSample(const testing::AudioSignal& sig) {
    double peak = 0.0;
    for (const float s : sig.samples()) {
        peak = std::max(peak, std::abs(static_cast<double>(s)));
    }
    return peak;
}

double rms(const testing::AudioSignal& sig, std::size_t start, std::size_t count,
           std::uint32_t ch) {
    const std::uint32_t channels = sig.format().channelCount();
    double acc = 0.0;
    for (std::size_t f = start; f < start + count; ++f) {
        const double v = static_cast<double>(sig.samples()[f * channels + ch]);
        acc += v * v;
    }
    return std::sqrt(acc / static_cast<double>(count));
}

LimiterProcessor::Parameters baseParams() {
    LimiterProcessor::Parameters p;
    p.ceilingDbtp = -1.0;
    p.lookAhead = microseconds{1500};  // 1.5 ms -> 72 frames @48k
    p.release = milliseconds{100};
    return p;
}

TEST(LimiterProcessorTest, OutputTruePeakStaysBelowCeiling) {
    auto lim = LimiterProcessor::create(baseParams(), monoFormat());
    ASSERT_TRUE(lim.hasValue());

    // A signal whose *true* peak (0 dBTP) sits above the -1 dBTP ceiling even
    // though its sample peak (0.707) is well under it.
    auto sig = makeInterSampleSine(monoFormat(), secToFrames(0.5), 1.0);
    const double inTp = measureTruePeakDbtp(sig);

    AudioBuffer buf = sig.view();
    ASSERT_TRUE(lim.value().process(buf).hasValue());

    const double outTp = measureTruePeakDbtp(sig);
    const double ceilingLinear = std::pow(10.0, baseParams().ceilingDbtp / 20.0);
    const double outSamplePeak = maxAbsSample(sig);
    std::printf(
        "[limiter] input TP=%.3f dBTP, output TP=%.3f dBTP, ceiling=%.3f dBTP\n",
        inTp, outTp, baseParams().ceilingDbtp);

    EXPECT_GT(inTp, baseParams().ceilingDbtp);          // input really was over
    EXPECT_LE(outTp, baseParams().ceilingDbtp + 0.15);  // true-peak controlled
    EXPECT_LE(outSamplePeak, ceilingLinear + 1e-4);     // no sample overshoot
}

TEST(LimiterProcessorTest, BelowCeilingPassesThroughDelayedByLookAhead) {
    auto lim = LimiterProcessor::create(baseParams(), monoFormat());
    ASSERT_TRUE(lim.hasValue());
    const std::size_t latency = lim.value().latencyFrames();
    ASSERT_GT(latency, 0u);

    // -6 dBFS tone: comfortably under the ceiling, so gain stays at unity and
    // only the look-ahead delay should be visible.
    const std::size_t frames = secToFrames(0.2);
    auto sig = testing::makeSine(monoFormat(), frames, 997.0, -6.0);
    const std::vector<float> original = sig.samples();

    AudioBuffer buf = sig.view();
    ASSERT_TRUE(lim.value().process(buf).hasValue());

    // First `latency` samples are the primed (silent) delay line.
    for (std::size_t i = 0; i < latency; ++i) {
        EXPECT_NEAR(sig.samples()[i], 0.0F, 1e-6F) << "prime at " << i;
    }
    // The rest is the input, unchanged, shifted by exactly the look-ahead.
    for (std::size_t i = latency; i < frames; ++i) {
        EXPECT_NEAR(sig.samples()[i], original[i - latency], 1e-6F)
            << "delayed content at " << i;
    }
}

TEST(LimiterProcessorTest, ChannelsAreLinked) {
    auto lim = LimiterProcessor::create(baseParams(), stereoFormat());
    ASSERT_TRUE(lim.hasValue());
    const std::size_t latency = lim.value().latencyFrames();

    // L: loud inter-sample sine over the ceiling. R: quiet tone well under it.
    const std::size_t frames = secToFrames(0.4);
    testing::AudioSignal sig{stereoFormat(), frames};
    auto loud = makeInterSampleSine(monoFormat(), frames, 1.0);
    auto quiet = testing::makeSine(monoFormat(), frames, 997.0, -20.0);
    for (std::size_t f = 0; f < frames; ++f) {
        sig.samples()[f * 2 + 0] = loud.samples()[f];
        sig.samples()[f * 2 + 1] = quiet.samples()[f];
    }
    const testing::AudioSignal input = sig;

    AudioBuffer buf = sig.view();
    ASSERT_TRUE(lim.value().process(buf).hasValue());

    // Compare output (delayed) to input over a steady interior window.
    const std::size_t start = secToFrames(0.2);
    const std::size_t count = secToFrames(0.1);
    const double gainL =
        rms(sig, start + latency, count, 0) / rms(input, start, count, 0);
    const double gainR =
        rms(sig, start + latency, count, 1) / rms(input, start, count, 1);

    EXPECT_LT(gainL, 0.95);           // L was attenuated by the limiter
    EXPECT_NEAR(gainR, gainL, 0.01);  // R got the exact same (linked) gain
}

TEST(LimiterProcessorTest, ReportedLatencyMatchesLookAhead) {
    {
        auto lim = LimiterProcessor::create(baseParams(), monoFormat());
        ASSERT_TRUE(lim.hasValue());
        EXPECT_EQ(lim.value().latencyFrames(), 72u);  // 1.5 ms @ 48 kHz
        const double latencyMs =
            std::chrono::duration_cast<
                std::chrono::duration<double, std::milli>>(lim.value().latency())
                .count();
        EXPECT_NEAR(latencyMs, 1.5, 0.05);
    }
    {
        auto p = baseParams();
        p.lookAhead = milliseconds{5};
        auto lim = LimiterProcessor::create(p, monoFormat());
        ASSERT_TRUE(lim.hasValue());
        EXPECT_EQ(lim.value().latencyFrames(), 240u);  // 5 ms @ 48 kHz
    }
}

TEST(LimiterProcessorTest, InvalidParametersRejected) {
    {
        auto p = baseParams();
        p.ceilingDbtp = 1.0;  // above full scale
        EXPECT_FALSE(LimiterProcessor::create(p, monoFormat()).hasValue());
    }
    {
        auto p = baseParams();
        p.ceilingDbtp = std::numeric_limits<double>::quiet_NaN();
        EXPECT_FALSE(LimiterProcessor::create(p, monoFormat()).hasValue());
    }
    {
        auto p = baseParams();
        p.lookAhead = microseconds{0};
        EXPECT_FALSE(LimiterProcessor::create(p, monoFormat()).hasValue());
    }
    {
        auto p = baseParams();
        p.release = milliseconds{-1};
        EXPECT_FALSE(LimiterProcessor::create(p, monoFormat()).hasValue());
    }
}

TEST(LimiterProcessorTest, NonFiniteInputSampleRejected) {
    auto lim = LimiterProcessor::create(baseParams(), monoFormat());
    ASSERT_TRUE(lim.hasValue());
    std::vector<float> storage(256, 0.2F);
    storage[100] = std::numeric_limits<float>::infinity();
    AudioBuffer buf{storage.data(), storage.size(), monoFormat()};
    const auto r = lim.value().process(buf);
    ASSERT_FALSE(r.hasValue());
    EXPECT_EQ(r.error().code(), core::ErrorCode::InvalidArgument);
}

TEST(LimiterProcessorTest, MismatchedFormatRejected) {
    auto lim = LimiterProcessor::create(baseParams(), monoFormat());
    ASSERT_TRUE(lim.hasValue());
    std::vector<float> storage(256, 0.1F);
    AudioBuffer buf{storage.data(), storage.size() / 2, stereoFormat()};
    EXPECT_FALSE(lim.value().process(buf).hasValue());
}

TEST(LimiterProcessorTest, EmptyBufferIsNoOp) {
    auto lim = LimiterProcessor::create(baseParams(), monoFormat());
    ASSERT_TRUE(lim.hasValue());
    AudioBuffer buf{nullptr, 0, monoFormat()};
    EXPECT_TRUE(lim.value().process(buf).hasValue());
}

// reset() must clear the look-ahead delay line (which still holds the tail of a
// previous block) plus the gain/detector state, so the next block is processed
// exactly as a freshly created instance would — no leaked tail bleeding out.
TEST(LimiterProcessorTest, ResetClearsStateToFreshInstance) {
    auto hot = LimiterProcessor::create(baseParams(), monoFormat());
    auto fresh = LimiterProcessor::create(baseParams(), monoFormat());
    ASSERT_TRUE(hot.hasValue());
    ASSERT_TRUE(fresh.hasValue());

    // Drive `hot` with a loud over-ceiling block: the delay line ends full of
    // loud samples and the smoothed gain is well below unity.
    auto loud = makeInterSampleSine(monoFormat(), secToFrames(0.2), 1.0);
    AudioBuffer loudBuf = loud.view();
    ASSERT_TRUE(hot.value().process(loudBuf).hasValue());
    hot.value().reset();

    // Same probe through the reset instance and a fresh instance.
    auto probeHot = testing::makeSine(monoFormat(), secToFrames(0.2), 997.0, -6.0);
    auto probeFresh =
        testing::makeSine(monoFormat(), secToFrames(0.2), 997.0, -6.0);
    AudioBuffer bufHot = probeHot.view();
    AudioBuffer bufFresh = probeFresh.view();
    ASSERT_TRUE(hot.value().process(bufHot).hasValue());
    ASSERT_TRUE(fresh.value().process(bufFresh).hasValue());

    for (std::size_t i = 0; i < probeHot.samples().size(); ++i) {
        EXPECT_EQ(probeHot.samples()[i], probeFresh.samples()[i]) << "at " << i;
    }
}

// A positive oversample safety margin targets ceiling - margin, so the output
// true-peak lands below the raw ceiling by roughly the margin — giving headroom
// for the detector's ~0.7 dB inter-sample underestimate (CLAUDE.md §9).
TEST(LimiterProcessorTest, SafetyMarginLowersOutputTruePeak) {
    auto params = baseParams();
    params.oversampleSafetyMarginDb = 3.0;  // target -4 dBTP instead of -1
    auto lim = LimiterProcessor::create(params, monoFormat());
    ASSERT_TRUE(lim.hasValue());

    auto sig = makeInterSampleSine(monoFormat(), secToFrames(0.5), 1.0);
    AudioBuffer buf = sig.view();
    ASSERT_TRUE(lim.value().process(buf).hasValue());

    const double outTp = measureTruePeakDbtp(sig);
    const double target = params.ceilingDbtp - params.oversampleSafetyMarginDb;
    std::printf("[limiter] margin=%.1f dB, output TP=%.3f dBTP, target=%.3f\n",
                params.oversampleSafetyMarginDb, outTp, target);
    EXPECT_LE(outTp, target + 0.15);            // controlled to the lowered target
    EXPECT_LT(outTp, params.ceilingDbtp - 1.0);  // clearly below the raw ceiling
}

TEST(LimiterProcessorTest, NegativeSafetyMarginRejected) {
    auto p = baseParams();
    p.oversampleSafetyMarginDb = -0.5;
    EXPECT_FALSE(LimiterProcessor::create(p, monoFormat()).hasValue());

    p.oversampleSafetyMarginDb = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(LimiterProcessor::create(p, monoFormat()).hasValue());
}

}  // namespace
}  // namespace creator::audio_dsp
