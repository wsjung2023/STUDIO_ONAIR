#include "audio_dsp/LoudnessNormalizer.h"

#include "audio_dsp/AudioFormat.h"
#include "audio_dsp/LoudnessMeter.h"
#include "audio_dsp/support/SyntheticAudio.h"
#include "core/AppError.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <vector>

namespace creator::audio_dsp {
namespace {

AudioFormat stereoFormat() { return AudioFormat::create(48'000, 2).value(); }

constexpr std::size_t secToFrames(double seconds) {
    return static_cast<std::size_t>(seconds * 48'000.0);
}

double maxAbs(const std::vector<float>& samples) {
    double peak = 0.0;
    for (const float s : samples) {
        peak = std::max(peak, std::abs(static_cast<double>(s)));
    }
    return peak;
}

LoudnessNormalizer makeNormalizer(double targetLufs = -14.0,
                                  double ceilingDbtp = -1.0) {
    LoudnessNormalizer::Parameters p;
    p.targetLufs = targetLufs;
    p.truePeakCeilingDbtp = ceilingDbtp;
    auto n = LoudnessNormalizer::create(p);
    EXPECT_TRUE(n.hasValue());
    return std::move(n).value();
}

TEST(LoudnessNormalizerTest, BoostsQuietSignalUpToTarget) {
    auto norm = makeNormalizer(-14.0, -1.0);
    // A stereo 1 kHz sine at -23 dBFS reads ~-23 LUFS (identical channels add
    // +3 LU, offsetting the sine's ~-3 dB K-weighted crest).
    auto sig = testing::makeSine(stereoFormat(), secToFrames(2.0), 1000.0, -23.0);

    auto r = norm.normalize(sig.samples(), stereoFormat());
    ASSERT_TRUE(r.hasValue());
    const NormalizationResult& res = r.value();
    std::printf(
        "[normalize -23->-14] before=%.3f LUFS gain=%.3f dB after=%.3f LUFS "
        "tp=%.3f dBTP\n",
        res.measuredLufsBefore, res.appliedGainDb, res.achievedLufsAfter,
        res.truePeakAfterDbtp);

    EXPECT_TRUE(res.normalized);
    EXPECT_NEAR(res.measuredLufsBefore, -23.0, 1.5);
    EXPECT_GT(res.appliedGainDb, 0.0);                    // boosted
    EXPECT_NEAR(res.achievedLufsAfter, -14.0, 0.5);       // hit target
    EXPECT_LE(res.truePeakAfterDbtp, -1.0 + 0.15);        // ceiling respected
}

TEST(LoudnessNormalizerTest, AttenuatesHotSignalDownToTarget) {
    auto norm = makeNormalizer(-14.0, -1.0);
    // Hot stereo sine ~-6 LUFS.
    auto sig = testing::makeSine(stereoFormat(), secToFrames(2.0), 1000.0, -6.0);

    auto r = norm.normalize(sig.samples(), stereoFormat());
    ASSERT_TRUE(r.hasValue());
    const NormalizationResult& res = r.value();
    std::printf(
        "[normalize -6->-14] before=%.3f LUFS gain=%.3f dB after=%.3f LUFS "
        "tp=%.3f dBTP\n",
        res.measuredLufsBefore, res.appliedGainDb, res.achievedLufsAfter,
        res.truePeakAfterDbtp);

    EXPECT_TRUE(res.normalized);
    EXPECT_NEAR(res.measuredLufsBefore, -6.0, 1.5);
    EXPECT_LT(res.appliedGainDb, 0.0);                    // attenuated
    EXPECT_NEAR(res.achievedLufsAfter, -14.0, 0.5);       // hit target
    EXPECT_LE(res.truePeakAfterDbtp, -1.0 + 0.15);        // no overshoot
}

TEST(LoudnessNormalizerTest, PureSilenceIsNotBoosted) {
    auto norm = makeNormalizer(-14.0, -1.0);
    auto sig = testing::makeSilence(stereoFormat(), secToFrames(1.0));

    auto r = norm.normalize(sig.samples(), stereoFormat());
    ASSERT_TRUE(r.hasValue());
    const NormalizationResult& res = r.value();

    EXPECT_FALSE(res.normalized);
    EXPECT_DOUBLE_EQ(res.appliedGainDb, 0.0);
    EXPECT_FALSE(std::isfinite(res.measuredLufsBefore));  // kNoMeasurement
    // Signal remains silent — nothing was amplified.
    EXPECT_NEAR(maxAbs(sig.samples()), 0.0, 1e-9);
}

TEST(LoudnessNormalizerTest, BelowNoiseFloorIsNotBoosted) {
    auto norm = makeNormalizer(-14.0, -1.0);
    // ~-65 LUFS: measurable (above the -70 gate) but below the -60 noise floor,
    // so boosting it +50 dB would only amplify noise. Left untouched.
    auto sig = testing::makeSine(stereoFormat(), secToFrames(1.0), 1000.0, -65.0);
    const double before = maxAbs(sig.samples());

    auto r = norm.normalize(sig.samples(), stereoFormat());
    ASSERT_TRUE(r.hasValue());
    const NormalizationResult& res = r.value();

    EXPECT_FALSE(res.normalized);
    EXPECT_DOUBLE_EQ(res.appliedGainDb, 0.0);
    const double after = maxAbs(sig.samples());
    EXPECT_NEAR(after, before, before * 0.01 + 1e-9);  // not amplified
}

TEST(LoudnessNormalizerTest, RejectsInvalidParameters) {
    {
        LoudnessNormalizer::Parameters p;
        p.targetLufs = std::numeric_limits<double>::quiet_NaN();
        EXPECT_FALSE(LoudnessNormalizer::create(p).hasValue());
    }
    {
        LoudnessNormalizer::Parameters p;
        p.targetLufs = 3.0;  // positive LUFS is nonsensical
        EXPECT_FALSE(LoudnessNormalizer::create(p).hasValue());
    }
    {
        LoudnessNormalizer::Parameters p;
        p.truePeakCeilingDbtp = 1.0;  // above full scale
        EXPECT_FALSE(LoudnessNormalizer::create(p).hasValue());
    }
    {
        LoudnessNormalizer::Parameters p;
        p.truePeakCeilingDbtp = std::numeric_limits<double>::infinity();
        EXPECT_FALSE(LoudnessNormalizer::create(p).hasValue());
    }
}

TEST(LoudnessNormalizerTest, RejectsNonFiniteInput) {
    auto norm = makeNormalizer();
    auto sig = testing::makeSine(stereoFormat(), secToFrames(0.5), 1000.0, -20.0);
    sig.samples()[5000] = std::numeric_limits<float>::infinity();

    auto r = norm.normalize(sig.samples(), stereoFormat());
    ASSERT_FALSE(r.hasValue());
    EXPECT_EQ(r.error().code(), core::ErrorCode::InvalidArgument);
}

TEST(LoudnessNormalizerTest, RejectsNon48kFormat) {
    auto norm = makeNormalizer();
    const AudioFormat fmt = AudioFormat::create(44'100, 2).value();
    std::vector<float> samples(secToFrames(0.5) * 2, 0.1F);

    auto r = norm.normalize(samples, fmt);
    EXPECT_FALSE(r.hasValue());  // LoudnessMeter is 48 kHz-only
}

}  // namespace
}  // namespace creator::audio_dsp
