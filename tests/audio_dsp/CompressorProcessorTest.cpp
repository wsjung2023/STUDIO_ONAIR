#include "audio_dsp/CompressorProcessor.h"

#include "audio_dsp/AudioBuffer.h"
#include "audio_dsp/AudioFormat.h"
#include "audio_dsp/support/SyntheticAudio.h"
#include "core/AppError.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace creator::audio_dsp {
namespace {

using std::chrono::milliseconds;

AudioFormat monoFormat() { return AudioFormat::create(48'000, 1).value(); }
AudioFormat stereoFormat() { return AudioFormat::create(48'000, 2).value(); }

constexpr std::size_t secToFrames(double seconds) {
    return static_cast<std::size_t>(seconds * 48'000.0);
}

// Peak magnitude of channel `ch` over frames [start, start+count).
double peakAbs(const testing::AudioSignal& sig, std::size_t start,
               std::size_t count, std::uint32_t ch = 0) {
    const std::uint32_t channels = sig.format().channelCount();
    const std::vector<float>& s = sig.samples();
    double peak = 0.0;
    for (std::size_t f = start; f < start + count; ++f) {
        peak = std::max(peak, std::abs(static_cast<double>(s[f * channels + ch])));
    }
    return peak;
}

// RMS of channel `ch` over frames [start, start+count).
double rms(const testing::AudioSignal& sig, std::size_t start,
           std::size_t count, std::uint32_t ch = 0) {
    const std::uint32_t channels = sig.format().channelCount();
    const std::vector<float>& s = sig.samples();
    double acc = 0.0;
    for (std::size_t f = start; f < start + count; ++f) {
        const double v = static_cast<double>(s[f * channels + ch]);
        acc += v * v;
    }
    return std::sqrt(acc / static_cast<double>(count));
}

CompressorProcessor::Parameters baseParams() {
    CompressorProcessor::Parameters p;
    p.thresholdDb = -20.0;
    p.ratio = 4.0;
    p.kneeWidthDb = 0.0;  // hard knee: exact static law
    p.attack = milliseconds{5};
    p.release = milliseconds{50};
    p.makeupGainDb = 0.0;
    p.detection = CompressorProcessor::Detection::Peak;
    return p;
}

TEST(CompressorProcessorTest, BelowThresholdSignalUnchanged) {
    auto params = baseParams();
    params.kneeWidthDb = 6.0;  // lower knee edge = -23 dBFS
    auto comp = CompressorProcessor::create(params);
    ASSERT_TRUE(comp.hasValue());

    // -40 dBFS is well below the knee: nothing should be touched.
    auto sig = testing::makeSine(monoFormat(), secToFrames(0.2), 1000.0, -40.0);
    const std::vector<float> original = sig.samples();
    AudioBuffer buf = sig.view();
    ASSERT_TRUE(comp.value().process(buf).hasValue());

    for (std::size_t i = 0; i < original.size(); ++i) {
        EXPECT_NEAR(sig.samples()[i], original[i], 1e-5F) << "at " << i;
    }
}

TEST(CompressorProcessorTest, SteadyToneGainReductionMatchesLaw) {
    auto params = baseParams();  // threshold -20, ratio 4, hard knee
    auto comp = CompressorProcessor::create(params);
    ASSERT_TRUE(comp.hasValue());

    constexpr double kToneDbfs = -8.0;
    auto sig = testing::makeSine(monoFormat(), secToFrames(1.0), 1000.0, kToneDbfs);
    const double inputPeak = peakAbs(sig, 0, secToFrames(1.0));

    AudioBuffer buf = sig.view();
    ASSERT_TRUE(comp.value().process(buf).hasValue());

    // Measure the steady state over the last 200 ms (many cycles), well past
    // the 5 ms attack.
    const std::size_t start = secToFrames(0.8);
    const std::size_t count = secToFrames(0.2);
    const double outPeak = peakAbs(sig, start, count);

    const double measuredGrDb = 20.0 * std::log10(inputPeak / outPeak);
    const double theoreticalGrDb =
        (kToneDbfs - params.thresholdDb) * (1.0 - 1.0 / params.ratio);
    std::printf("[compressor] measured GR=%.3f dB, theoretical GR=%.3f dB\n",
                measuredGrDb, theoreticalGrDb);
    EXPECT_NEAR(measuredGrDb, theoreticalGrDb, 1.5);
}

TEST(CompressorProcessorTest, AttackReducesAndReleaseRecovers) {
    auto params = baseParams();
    params.attack = milliseconds{10};
    params.release = milliseconds{80};
    auto comp = CompressorProcessor::create(params);
    ASSERT_TRUE(comp.hasValue());

    // Phase A: loud tone above threshold. Phase B: quiet tone below threshold.
    const std::size_t phase = secToFrames(0.4);
    auto loud = testing::makeSine(monoFormat(), phase, 1000.0, -6.0);
    auto quiet = testing::makeSine(monoFormat(), phase, 1000.0, -45.0);
    testing::AudioSignal sig{monoFormat(), phase * 2};
    for (std::size_t i = 0; i < phase; ++i) {
        sig.samples()[i] = loud.samples()[i];
        sig.samples()[phase + i] = quiet.samples()[i];
    }
    // Keep untouched copies to form gain = out/in per window.
    const testing::AudioSignal input = sig;

    AudioBuffer buf = sig.view();
    ASSERT_TRUE(comp.value().process(buf).hasValue());

    const std::size_t win = secToFrames(0.005);  // 5 ms windows
    auto gainAt = [&](std::size_t frame) {
        return rms(sig, frame, win) / rms(input, frame, win);
    };

    // --- attack: gain falls from ~1 toward the compressed steady state -------
    const double gEarlyA = gainAt(0);
    const double gSteadyA = gainAt(phase - win - 1);
    EXPECT_GT(gEarlyA, 0.85);          // barely touched at onset
    EXPECT_LT(gSteadyA, 0.75);         // clearly compressed by end of phase A
    // One attack time-constant in, it should have covered a good part of the
    // drop (rough time constant, not sample-exact).
    const double gAtAttack = gainAt(secToFrames(0.010));
    EXPECT_LT(gAtAttack, gEarlyA - 0.4 * (gEarlyA - gSteadyA));

    // --- release: gain climbs back toward unity once the signal drops --------
    const double gEarlyB = gainAt(phase + 1);
    const double gLateB = gainAt(2 * phase - win - 1);
    EXPECT_LT(gEarlyB, 0.9);           // still holding compression at the drop
    EXPECT_GT(gLateB, 0.95);           // fully recovered after >> release
    const double gAtRelease = gainAt(phase + secToFrames(0.080));
    EXPECT_GT(gAtRelease, gEarlyB + 0.4 * (1.0 - gEarlyB));
}

TEST(CompressorProcessorTest, SoftKneeIsContinuousAcrossThreshold) {
    CompressorProcessor::Parameters params = baseParams();
    params.kneeWidthDb = 12.0;  // wide knee straddling the threshold
    params.attack = milliseconds{1};

    // Sweep DC input level through the knee; measure settled gain each time.
    double prevGain = 1.0;
    bool first = true;
    for (double levelDb = -40.0; levelDb <= -2.0; levelDb += 0.25) {
        auto comp = CompressorProcessor::create(params);
        ASSERT_TRUE(comp.hasValue());
        const float dc = static_cast<float>(std::pow(10.0, levelDb / 20.0));
        std::vector<float> storage(secToFrames(0.05), dc);
        AudioBuffer buf{storage.data(), storage.size(), monoFormat()};
        ASSERT_TRUE(comp.value().process(buf).hasValue());
        const double gain = static_cast<double>(storage.back()) / dc;

        EXPECT_LE(gain, 1.0 + 1e-4);  // downward compressor never boosts
        if (!first) {
            // A jump would betray a discontinuity at the knee; the curve must
            // move smoothly in small steps.
            EXPECT_LT(std::abs(gain - prevGain), 0.03)
                << "discontinuity near " << levelDb << " dBFS";
            EXPECT_LE(gain, prevGain + 1e-4);  // monotonically non-increasing
        }
        prevGain = gain;
        first = false;
    }
}

TEST(CompressorProcessorTest, MakeupGainIsApplied) {
    auto params = baseParams();
    params.kneeWidthDb = 6.0;
    params.makeupGainDb = 6.020599913279624;  // +6 dB ~ x2
    auto comp = CompressorProcessor::create(params);
    ASSERT_TRUE(comp.hasValue());

    // Below threshold: no compression, so output = input * makeup.
    const float dc = static_cast<float>(std::pow(10.0, -40.0 / 20.0));
    std::vector<float> storage(secToFrames(0.05), dc);
    AudioBuffer buf{storage.data(), storage.size(), monoFormat()};
    ASSERT_TRUE(comp.value().process(buf).hasValue());

    for (const float s : storage) {
        EXPECT_NEAR(s, dc * 2.0F, 1e-3F);
    }
}

TEST(CompressorProcessorTest, LinkedStereoSharesGain) {
    auto params = baseParams();
    auto comp = CompressorProcessor::create(params);
    ASSERT_TRUE(comp.hasValue());

    // Loud on L (above threshold), quiet on R (below). Linked gain means R is
    // attenuated by the same factor L is, preserving the image.
    const std::size_t frames = secToFrames(0.5);
    testing::AudioSignal sig{stereoFormat(), frames};
    auto loud = testing::makeSine(monoFormat(), frames, 1000.0, -6.0);
    auto quiet = testing::makeSine(monoFormat(), frames, 1000.0, -30.0);
    for (std::size_t f = 0; f < frames; ++f) {
        sig.samples()[f * 2 + 0] = loud.samples()[f];
        sig.samples()[f * 2 + 1] = quiet.samples()[f];
    }
    const testing::AudioSignal input = sig;

    AudioBuffer buf = sig.view();
    ASSERT_TRUE(comp.value().process(buf).hasValue());

    const std::size_t start = secToFrames(0.3);
    const std::size_t count = secToFrames(0.15);
    const double gainL = rms(sig, start, count, 0) / rms(input, start, count, 0);
    const double gainR = rms(sig, start, count, 1) / rms(input, start, count, 1);
    EXPECT_LT(gainL, 0.75);            // L was compressed
    EXPECT_NEAR(gainR, gainL, 0.02);   // R attenuated by the same linked gain
}

TEST(CompressorProcessorTest, InvalidParametersRejected) {
    {
        auto p = baseParams();
        p.ratio = 0.5;  // < 1
        EXPECT_FALSE(CompressorProcessor::create(p).hasValue());
    }
    {
        auto p = baseParams();
        p.kneeWidthDb = -1.0;
        EXPECT_FALSE(CompressorProcessor::create(p).hasValue());
    }
    {
        auto p = baseParams();
        p.attack = milliseconds{0};
        EXPECT_FALSE(CompressorProcessor::create(p).hasValue());
    }
    {
        auto p = baseParams();
        p.release = milliseconds{-1};
        EXPECT_FALSE(CompressorProcessor::create(p).hasValue());
    }
    {
        auto p = baseParams();
        p.thresholdDb = std::numeric_limits<double>::quiet_NaN();
        EXPECT_FALSE(CompressorProcessor::create(p).hasValue());
    }
    {
        auto p = baseParams();
        p.makeupGainDb = std::numeric_limits<double>::infinity();
        EXPECT_FALSE(CompressorProcessor::create(p).hasValue());
    }
}

TEST(CompressorProcessorTest, NonFiniteInputSampleRejected) {
    auto comp = CompressorProcessor::create(baseParams());
    ASSERT_TRUE(comp.hasValue());
    std::vector<float> storage(64, 0.2F);
    storage[20] = std::numeric_limits<float>::quiet_NaN();
    AudioBuffer buf{storage.data(), storage.size(), monoFormat()};
    const auto r = comp.value().process(buf);
    ASSERT_FALSE(r.hasValue());
    EXPECT_EQ(r.error().code(), core::ErrorCode::InvalidArgument);
}

TEST(CompressorProcessorTest, EmptyBufferIsNoOp) {
    auto comp = CompressorProcessor::create(baseParams());
    ASSERT_TRUE(comp.hasValue());
    AudioBuffer buf{nullptr, 0, monoFormat()};
    EXPECT_TRUE(comp.value().process(buf).hasValue());
}

// Denormal flush: after a loud block excites the envelope follower, a long
// block of *exact* silence must drive the persisted envelope to EXACTLY 0.0 —
// not a lingering subnormal double that would stall the x86 FPU (CLAUDE.md §9).
TEST(CompressorProcessorTest, EnvelopeFlushesToExactZeroOnSilence) {
    auto params = baseParams();
    params.release = milliseconds{5};  // quick decay keeps the test short
    auto comp = CompressorProcessor::create(params);
    ASSERT_TRUE(comp.hasValue());

    // Loud tone lifts the envelope well above zero.
    auto loud = testing::makeSine(monoFormat(), secToFrames(0.1), 1000.0, -6.0);
    AudioBuffer loudBuf = loud.view();
    ASSERT_TRUE(comp.value().process(loudBuf).hasValue());
    ASSERT_GT(comp.value().envelope(), 0.0);

    // 1 s of exact silence: the flush must collapse the decaying envelope to 0.
    std::vector<float> silence(secToFrames(1.0), 0.0F);
    AudioBuffer silenceBuf{silence.data(), silence.size(), monoFormat()};
    ASSERT_TRUE(comp.value().process(silenceBuf).hasValue());

    EXPECT_EQ(comp.value().envelope(), 0.0);  // exact, not a subnormal
}

// reset() must clear all carried state so the next block is processed exactly
// as a freshly created instance would — no leaked compression tail.
TEST(CompressorProcessorTest, ResetClearsStateToFreshInstance) {
    auto params = baseParams();
    auto hot = CompressorProcessor::create(params);
    auto fresh = CompressorProcessor::create(params);
    ASSERT_TRUE(hot.hasValue());
    ASSERT_TRUE(fresh.hasValue());

    // Drive `hot` hard so its envelope/gain state is far from a clean start.
    auto loud = testing::makeSine(monoFormat(), secToFrames(0.2), 1000.0, -3.0);
    AudioBuffer loudBuf = loud.view();
    ASSERT_TRUE(hot.value().process(loudBuf).hasValue());
    hot.value().reset();

    // Same probe through the reset instance and a fresh instance.
    auto probeHot = testing::makeSine(monoFormat(), secToFrames(0.2), 1000.0, -3.0);
    auto probeFresh =
        testing::makeSine(monoFormat(), secToFrames(0.2), 1000.0, -3.0);
    AudioBuffer bufHot = probeHot.view();
    AudioBuffer bufFresh = probeFresh.view();
    ASSERT_TRUE(hot.value().process(bufHot).hasValue());
    ASSERT_TRUE(fresh.value().process(bufFresh).hasValue());

    for (std::size_t i = 0; i < probeHot.samples().size(); ++i) {
        EXPECT_EQ(probeHot.samples()[i], probeFresh.samples()[i]) << "at " << i;
    }
}

}  // namespace
}  // namespace creator::audio_dsp
