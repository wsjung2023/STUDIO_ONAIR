#include "audio_dsp/LoudnessMeter.h"

#include "audio_dsp/AudioBuffer.h"
#include "audio_dsp/AudioFormat.h"
#include "audio_dsp/support/SyntheticAudio.h"
#include "core/AppError.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

namespace creator::audio_dsp {
namespace {

AudioFormat stereo48k() { return AudioFormat::create(48'000, 2).value(); }
AudioFormat mono48k() { return AudioFormat::create(48'000, 1).value(); }

constexpr std::size_t kSecondsToFrames(double seconds) {
    return static_cast<std::size_t>(seconds * 48'000.0);
}

// Feed a whole owning signal to a meter in one block.
[[nodiscard]] core::Result<void> feed(LoudnessMeter& meter,
                                      testing::AudioSignal& signal) {
    AudioBuffer view = signal.view();
    return meter.addBlock(view);
}

TEST(LoudnessMeterTest, StereoSineMinus23IsMinus23Lufs) {
    auto meter = LoudnessMeter::create(stereo48k());
    ASSERT_TRUE(meter.hasValue());
    auto signal = testing::makeSine(stereo48k(), kSecondsToFrames(10.0), 1000.0, -23.0);
    ASSERT_TRUE(feed(meter.value(), signal).hasValue());

    EXPECT_NEAR(meter.value().integratedLufs(), -23.0, 0.1);
    EXPECT_NEAR(meter.value().momentaryLufs(), -23.0, 0.1);
    EXPECT_NEAR(meter.value().shortTermLufs(), -23.0, 0.1);
}

TEST(LoudnessMeterTest, LinearityMinus33IsMinus33Lufs) {
    auto meter = LoudnessMeter::create(stereo48k());
    ASSERT_TRUE(meter.hasValue());
    auto signal = testing::makeSine(stereo48k(), kSecondsToFrames(10.0), 1000.0, -33.0);
    ASSERT_TRUE(feed(meter.value(), signal).hasValue());

    EXPECT_NEAR(meter.value().integratedLufs(), -33.0, 0.1);
}

TEST(LoudnessMeterTest, DoublingAmplitudeAddsSixLU) {
    auto base = LoudnessMeter::create(stereo48k());
    auto louder = LoudnessMeter::create(stereo48k());
    ASSERT_TRUE(base.hasValue());
    ASSERT_TRUE(louder.hasValue());

    auto baseSig = testing::makeSine(stereo48k(), kSecondsToFrames(10.0), 1000.0, -33.0);
    auto loudSig = testing::makeSine(stereo48k(), kSecondsToFrames(10.0), 1000.0, -33.0);
    for (float& s : loudSig.samples()) {
        s *= 2.0F;
    }
    ASSERT_TRUE(feed(base.value(), baseSig).hasValue());
    ASSERT_TRUE(feed(louder.value(), loudSig).hasValue());

    EXPECT_NEAR(louder.value().integratedLufs() - base.value().integratedLufs(),
                6.0206, 0.05);
}

TEST(LoudnessMeterTest, DuplicatingMonoIntoStereoAddsThreeLU) {
    auto monoMeter = LoudnessMeter::create(mono48k());
    auto stereoMeter = LoudnessMeter::create(stereo48k());
    ASSERT_TRUE(monoMeter.hasValue());
    ASSERT_TRUE(stereoMeter.hasValue());

    auto monoSig = testing::makeSine(mono48k(), kSecondsToFrames(10.0), 1000.0, -23.0);
    auto stereoSig = testing::makeSine(stereo48k(), kSecondsToFrames(10.0), 1000.0, -23.0);
    ASSERT_TRUE(feed(monoMeter.value(), monoSig).hasValue());
    ASSERT_TRUE(feed(stereoMeter.value(), stereoSig).hasValue());

    EXPECT_NEAR(
        stereoMeter.value().integratedLufs() - monoMeter.value().integratedLufs(),
        3.0103, 0.05);
}

TEST(LoudnessMeterTest, AbsoluteGateExcludesBlocksBelowMinus70) {
    auto meter = LoudnessMeter::create(stereo48k());
    ASSERT_TRUE(meter.hasValue());

    auto body = testing::makeSine(stereo48k(), kSecondsToFrames(5.0), 1000.0, -23.0);
    ASSERT_TRUE(feed(meter.value(), body).hasValue());
    const double before = meter.value().integratedLufs();

    // A far-below-gate tail (~-85 dBFS -> ~-85 LUFS < -70 absolute gate). Every
    // pure-tail 400 ms block is discarded, so integrated stays at the body's
    // ~-23. Were the absolute gate absent, the near-silent blocks would halve
    // the mean power and drag integrated to ~-26 LUFS. The small residual is
    // the handful of 75%-overlap blocks straddling the loud/quiet boundary,
    // which legitimately sit above -70 and are kept.
    auto quiet = testing::makeSine(stereo48k(), kSecondsToFrames(5.0), 1000.0, -85.0);
    ASSERT_TRUE(feed(meter.value(), quiet).hasValue());
    const double after = meter.value().integratedLufs();

    EXPECT_NEAR(after, before, 0.25);  // gate working: not the ~3 LU drop
    EXPECT_NEAR(after, -23.0, 0.25);
}

TEST(LoudnessMeterTest, RelativeGateExcludesQuietTail) {
    auto meter = LoudnessMeter::create(stereo48k());
    ASSERT_TRUE(meter.hasValue());

    // Loud body at -23, quiet tail at -40 (17 LU below -> above absolute gate,
    // excluded only by the relative gate). Without relative gating the mix would
    // read well below -23.
    auto body = testing::makeSine(stereo48k(), kSecondsToFrames(10.0), 1000.0, -23.0);
    auto tail = testing::makeSine(stereo48k(), kSecondsToFrames(10.0), 1000.0, -40.0);
    ASSERT_TRUE(feed(meter.value(), body).hasValue());
    ASSERT_TRUE(feed(meter.value(), tail).hasValue());

    EXPECT_NEAR(meter.value().integratedLufs(), -23.0, 0.2);
}

TEST(LoudnessMeterTest, TruePeakCatchesInterSamplePeak) {
    auto meter = LoudnessMeter::create(mono48k());
    ASSERT_TRUE(meter.hasValue());

    // Sine at fs/4 with a pi/4 phase: every sample lands at +/-A/sqrt(2), so the
    // sample peak is 0.707*A while the true (continuous) peak is A. Crest falls
    // exactly on a 4x-oversampled phase.
    constexpr double kA = 0.9;
    const std::size_t frames = kSecondsToFrames(1.0);
    testing::AudioSignal signal{mono48k(), frames};
    std::vector<float>& out = signal.samples();
    for (std::size_t n = 0; n < frames; ++n) {
        const double phase =
            std::numbers::pi / 2.0 * static_cast<double>(n) + std::numbers::pi / 4.0;
        out[n] = static_cast<float>(kA * std::sin(phase));
    }
    ASSERT_TRUE(feed(meter.value(), signal).hasValue());

    const double samplePeakDb = 20.0 * std::log10(kA / std::numbers::sqrt2);
    const double truePeakDb = meter.value().truePeakDbtp();
    EXPECT_TRUE(std::isfinite(truePeakDb));
    // True peak must clearly exceed the sample-peak meter (by ~3 dB here).
    EXPECT_GT(truePeakDb, samplePeakDb + 1.5);
    // And it should approach the real crest 20*log10(0.9) ~ -0.915 dBTP.
    EXPECT_NEAR(truePeakDb, 20.0 * std::log10(kA), 0.5);
}

TEST(LoudnessMeterTest, ShortSignalHasNoMeasurement) {
    auto meter = LoudnessMeter::create(stereo48k());
    ASSERT_TRUE(meter.hasValue());

    // 200 ms < one 400 ms block: no gated block, no momentary window.
    auto shortSig = testing::makeSine(stereo48k(), kSecondsToFrames(0.2), 1000.0, -23.0);
    ASSERT_TRUE(feed(meter.value(), shortSig).hasValue());

    EXPECT_FALSE(std::isfinite(meter.value().integratedLufs()));
    EXPECT_FALSE(std::isfinite(meter.value().momentaryLufs()));
    EXPECT_FALSE(std::isfinite(meter.value().shortTermLufs()));
}

TEST(LoudnessMeterTest, RejectsNon48kFormat) {
    auto meter = LoudnessMeter::create(AudioFormat::create(44'100, 2).value());
    ASSERT_FALSE(meter.hasValue());
    EXPECT_EQ(meter.error().code(), core::ErrorCode::InvalidArgument);
}

TEST(LoudnessMeterTest, RejectsNonFiniteSample) {
    auto meter = LoudnessMeter::create(stereo48k());
    ASSERT_TRUE(meter.hasValue());
    auto signal = testing::makeSine(stereo48k(), kSecondsToFrames(0.5), 1000.0, -23.0);
    signal.samples()[100] = std::numeric_limits<float>::quiet_NaN();

    const auto result = feed(meter.value(), signal);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), core::ErrorCode::InvalidArgument);
}

TEST(LoudnessMeterTest, RejectsMismatchedFormat) {
    auto meter = LoudnessMeter::create(stereo48k());
    ASSERT_TRUE(meter.hasValue());
    auto monoSig = testing::makeSine(mono48k(), kSecondsToFrames(0.5), 1000.0, -23.0);
    const auto result = feed(meter.value(), monoSig);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), core::ErrorCode::InvalidArgument);
}

TEST(LoudnessMeterTest, ResetClearsMeasurement) {
    auto meter = LoudnessMeter::create(stereo48k());
    ASSERT_TRUE(meter.hasValue());
    auto signal = testing::makeSine(stereo48k(), kSecondsToFrames(5.0), 1000.0, -23.0);
    ASSERT_TRUE(feed(meter.value(), signal).hasValue());
    ASSERT_TRUE(std::isfinite(meter.value().integratedLufs()));

    meter.value().reset();
    EXPECT_FALSE(std::isfinite(meter.value().integratedLufs()));
    EXPECT_FALSE(std::isfinite(meter.value().truePeakDbtp()));
}

}  // namespace
}  // namespace creator::audio_dsp
