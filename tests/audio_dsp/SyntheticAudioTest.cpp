#include "audio_dsp/support/SyntheticAudio.h"

#include "audio_dsp/AudioFormat.h"

#include <gtest/gtest.h>

#include <cmath>

namespace creator::audio_dsp::testing {
namespace {

double rms(const std::vector<float>& samples) {
    double sum = 0.0;
    for (const float s : samples) {
        sum += static_cast<double>(s) * static_cast<double>(s);
    }
    return std::sqrt(sum / static_cast<double>(samples.size()));
}

double peak(const std::vector<float>& samples) {
    double p = 0.0;
    for (const float s : samples) {
        p = std::max(p, std::abs(static_cast<double>(s)));
    }
    return p;
}

TEST(SyntheticAudioTest, SilenceIsAllZeros) {
    const auto format = AudioFormat::create(48'000, 2).value();
    const AudioSignal signal = makeSilence(format, 256);
    EXPECT_EQ(signal.frameCount(), 256u);
    EXPECT_EQ(signal.samples().size(), 512u);
    for (const float s : signal.samples()) {
        EXPECT_FLOAT_EQ(s, 0.0F);
    }
}

TEST(SyntheticAudioTest, SineHasExpectedPeakAndRms) {
    const auto format = AudioFormat::create(48'000, 1).value();
    // −6 dBFS → amplitude 0.5011872; a full-wavelength-aligned span keeps RMS
    // near amplitude/sqrt(2).
    const double levelDbfs = -6.0;
    const double amplitude = std::pow(10.0, levelDbfs / 20.0);
    const AudioSignal signal = makeSine(format, 48'000, 1'000.0, levelDbfs);

    EXPECT_NEAR(peak(signal.samples()), amplitude, 1e-3);
    EXPECT_NEAR(rms(signal.samples()), amplitude / std::sqrt(2.0), 1e-3);
}

TEST(SyntheticAudioTest, SineIsDeterministic) {
    const auto format = AudioFormat::create(48'000, 1).value();
    const AudioSignal a = makeSine(format, 1'000, 440.0, -20.0);
    const AudioSignal b = makeSine(format, 1'000, 440.0, -20.0);
    EXPECT_EQ(a.samples(), b.samples());
}

}  // namespace
}  // namespace creator::audio_dsp::testing
