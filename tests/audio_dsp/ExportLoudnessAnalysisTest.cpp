#include "audio_dsp/ExportLoudnessAnalysis.h"

#include "audio_dsp/AudioFormat.h"
#include "audio_dsp/LoudnessMeter.h"
#include "audio_dsp/LoudnessNormalizer.h"
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

ExportLoudnessAnalyzer makeAnalyzer(double targetLufs = -14.0,
                                    double ceilingDbtp = -1.0) {
    ExportLoudnessAnalyzer::Parameters p;
    p.targetLufs = targetLufs;
    p.truePeakCeilingDbtp = ceilingDbtp;
    auto a = ExportLoudnessAnalyzer::create(p);
    EXPECT_TRUE(a.hasValue());
    return std::move(a).value();
}

// A -23 LUFS program with a -14 target must report roughly +9 dB of gain: this
// is the canonical R2 "음량 표준화" (loudness standardization) decision.
TEST(ExportLoudnessAnalysisTest, DecidesPositiveGainForQuietProgram) {
    auto analyzer = makeAnalyzer(-14.0, -1.0);
    // A stereo 1 kHz sine at -23 dBFS reads ~-23 LUFS (identical channels add
    // +3 LU, offsetting the sine's ~-3 dB K-weighted crest) — the same fixture
    // LoudnessNormalizerTest uses, so the two modules are compared like-for-like.
    auto sig = testing::makeSine(stereoFormat(), secToFrames(2.0), 1000.0, -23.0);
    const std::vector<float> before = sig.samples();  // copy to prove purity

    auto r = analyzer.analyze(sig.samples(), stereoFormat());
    ASSERT_TRUE(r.hasValue());
    const ExportLoudnessDecision& d = r.value();
    std::printf(
        "[export-analysis -23->-14] measured=%.3f LUFS gain=%.3f dB tp=%.3f "
        "dBTP normalize=%d\n",
        d.measuredLufs, d.gainDb, d.truePeakDbtp,
        static_cast<int>(d.shouldNormalize));

    EXPECT_TRUE(d.shouldNormalize);
    EXPECT_NEAR(d.measuredLufs, -23.0, 1.5);
    EXPECT_NEAR(d.gainDb, 9.0, 1.5);  // target(-14) - measured(-23) ≈ +9 dB
    EXPECT_DOUBLE_EQ(d.targetLufs, -14.0);
    EXPECT_DOUBLE_EQ(d.truePeakCeilingDbtp, -1.0);
    // The analyzer only MEASURES — the input buffer must be untouched.
    EXPECT_EQ(sig.samples(), before);
}

// The decided gain must equal the gain LoudnessNormalizer actually applies, so
// the two-pass exporter and the offline normalizer stay in lock step.
TEST(ExportLoudnessAnalysisTest, GainMatchesLoudnessNormalizer) {
    auto analyzer = makeAnalyzer(-14.0, -1.0);
    auto sig = testing::makeSine(stereoFormat(), secToFrames(2.0), 1000.0, -23.0);

    auto decision = analyzer.analyze(sig.samples(), stereoFormat());
    ASSERT_TRUE(decision.hasValue());

    LoudnessNormalizer::Parameters np;
    np.targetLufs = -14.0;
    np.truePeakCeilingDbtp = -1.0;
    auto normalizer = LoudnessNormalizer::create(np);
    ASSERT_TRUE(normalizer.hasValue());
    auto applied = normalizer.value().normalize(sig.samples(), stereoFormat());
    ASSERT_TRUE(applied.hasValue());

    // Same measurement, same decided gain.
    EXPECT_NEAR(decision.value().measuredLufs,
                applied.value().measuredLufsBefore, 1e-6);
    EXPECT_NEAR(decision.value().gainDb, applied.value().appliedGainDb, 1e-6);
    // And applying that gain drives the program to target.
    EXPECT_NEAR(applied.value().achievedLufsAfter, -14.0, 0.5);
}

TEST(ExportLoudnessAnalysisTest, StreamingMeterDecisionMatchesOwningBuffer) {
    auto analyzer = makeAnalyzer(-14.0, -1.0);
    auto sig = testing::makeSine(stereoFormat(), secToFrames(2.0), 1000.0, -23.0);
    auto owning = analyzer.analyze(sig.samples(), stereoFormat());
    ASSERT_TRUE(owning.hasValue());

    auto meter = LoudnessMeter::create(stereoFormat());
    ASSERT_TRUE(meter.hasValue());
    constexpr std::size_t kBlockFrames = 4'800;
    for (std::size_t frame = 0; frame < secToFrames(2.0);
         frame += kBlockFrames) {
        AudioBuffer block{sig.samples().data() + frame * 2U,
                          std::min(kBlockFrames, secToFrames(2.0) - frame),
                          stereoFormat()};
        ASSERT_TRUE(meter.value().addBlock(block).hasValue());
    }

    auto streaming = analyzer.decide(meter.value());
    ASSERT_TRUE(streaming.hasValue());
    EXPECT_NEAR(streaming.value().measuredLufs,
                owning.value().measuredLufs, 1e-9);
    EXPECT_NEAR(streaming.value().truePeakDbtp,
                owning.value().truePeakDbtp, 1e-9);
    EXPECT_NEAR(streaming.value().gainDb, owning.value().gainDb, 1e-9);
}

// A program already at target should decide ~0 dB of gain.
TEST(ExportLoudnessAnalysisTest, DecidesNearZeroGainWhenAlreadyAtTarget) {
    // Target -23 so an already ~-23 LUFS program needs no change.
    auto analyzer = makeAnalyzer(-23.0, -1.0);
    auto sig = testing::makeSine(stereoFormat(), secToFrames(2.0), 1000.0, -23.0);

    auto r = analyzer.analyze(sig.samples(), stereoFormat());
    ASSERT_TRUE(r.hasValue());
    const ExportLoudnessDecision& d = r.value();
    EXPECT_TRUE(d.shouldNormalize);
    EXPECT_NEAR(d.gainDb, 0.0, 1.5);
}

// Attenuation: a hot program decides negative gain.
TEST(ExportLoudnessAnalysisTest, DecidesNegativeGainForHotProgram) {
    auto analyzer = makeAnalyzer(-14.0, -1.0);
    auto sig = testing::makeSine(stereoFormat(), secToFrames(2.0), 1000.0, -6.0);

    auto r = analyzer.analyze(sig.samples(), stereoFormat());
    ASSERT_TRUE(r.hasValue());
    EXPECT_TRUE(r.value().shouldNormalize);
    EXPECT_LT(r.value().gainDb, 0.0);
}

// Silence is a documented no-op: no gain, no normalization, kNoMeasurement.
TEST(ExportLoudnessAnalysisTest, SilenceIsNoOp) {
    auto analyzer = makeAnalyzer(-14.0, -1.0);
    auto sig = testing::makeSilence(stereoFormat(), secToFrames(1.0));

    auto r = analyzer.analyze(sig.samples(), stereoFormat());
    ASSERT_TRUE(r.hasValue());
    const ExportLoudnessDecision& d = r.value();
    EXPECT_FALSE(d.shouldNormalize);
    EXPECT_DOUBLE_EQ(d.gainDb, 0.0);
    EXPECT_FALSE(std::isfinite(d.measuredLufs));  // kNoMeasurement
}

// Below the noise floor: measurable but too quiet to boost.
TEST(ExportLoudnessAnalysisTest, BelowNoiseFloorIsNoOp) {
    auto analyzer = makeAnalyzer(-14.0, -1.0);
    auto sig = testing::makeSine(stereoFormat(), secToFrames(1.0), 1000.0, -65.0);

    auto r = analyzer.analyze(sig.samples(), stereoFormat());
    ASSERT_TRUE(r.hasValue());
    EXPECT_FALSE(r.value().shouldNormalize);
    EXPECT_DOUBLE_EQ(r.value().gainDb, 0.0);
}

TEST(ExportLoudnessAnalysisTest, RejectsInvalidParameters) {
    {
        ExportLoudnessAnalyzer::Parameters p;
        p.targetLufs = std::numeric_limits<double>::quiet_NaN();
        EXPECT_FALSE(ExportLoudnessAnalyzer::create(p).hasValue());
    }
    {
        ExportLoudnessAnalyzer::Parameters p;
        p.targetLufs = 3.0;  // positive LUFS is nonsensical
        EXPECT_FALSE(ExportLoudnessAnalyzer::create(p).hasValue());
    }
    {
        ExportLoudnessAnalyzer::Parameters p;
        p.truePeakCeilingDbtp = 1.0;  // above full scale
        EXPECT_FALSE(ExportLoudnessAnalyzer::create(p).hasValue());
    }
}

TEST(ExportLoudnessAnalysisTest, RejectsNonFiniteInput) {
    auto analyzer = makeAnalyzer();
    auto sig = testing::makeSine(stereoFormat(), secToFrames(0.5), 1000.0, -20.0);
    sig.samples()[5000] = std::numeric_limits<float>::infinity();

    auto r = analyzer.analyze(sig.samples(), stereoFormat());
    ASSERT_FALSE(r.hasValue());
    EXPECT_EQ(r.error().code(), core::ErrorCode::InvalidArgument);
}

TEST(ExportLoudnessAnalysisTest, RejectsNon48kFormat) {
    auto analyzer = makeAnalyzer();
    const AudioFormat fmt = AudioFormat::create(44'100, 2).value();
    std::vector<float> samples(secToFrames(0.5) * 2, 0.1F);

    auto r = analyzer.analyze(samples, fmt);
    EXPECT_FALSE(r.hasValue());  // LoudnessMeter is 48 kHz-only
}

}  // namespace
}  // namespace creator::audio_dsp
