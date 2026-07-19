#include "audio_dsp/LoudnessMeter.h"

#include "core/AppError.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace creator::audio_dsp {

namespace {

// BS.1770 loudness offset: L = -0.691 + 10*log10(Sum_i G_i * z_i).
constexpr double kLoudnessOffset = -0.691;

// Gating thresholds (integrated only).
constexpr double kAbsoluteGateLkfs = -70.0;   // absolute gate
constexpr double kRelativeGateLu = -10.0;      // relative to gated mean

// Sub-block / window geometry (100 ms hop, 75% overlap).
constexpr std::size_t kSubBlocksPerMomentary = 4;    // 400 ms
constexpr std::size_t kSubBlocksPerShortTerm = 30;   // 3 s
constexpr std::size_t kRetainedSubBlocks = kSubBlocksPerShortTerm;

// Channel weights G_i by index under the documented L,R,C,Ls,Rs assumption.
[[nodiscard]] double channelWeight(std::uint32_t index) noexcept {
    // Left-surround (3) and Right-surround (4) are weighted 1.41; L/R/C and any
    // channel beyond the 5-channel model are weighted 1.0. LFE has no slot here.
    return (index == 3 || index == 4) ? 1.41 : 1.0;
}

// --- true-peak oversampling FIR (approximates BS.1770 Annex 2) --------------
// A 4x windowed-sinc interpolator. TODO(r2-06): swap for the exact BS.1770
// Annex 2 phase filter table if bit-exact dBTP is later required; this design
// already recovers inter-sample peaks a raw sample-peak meter misses.
constexpr int kOversampleFactor = 4;
constexpr int kTapsPerPhase = 12;
constexpr int kProtoLength = kOversampleFactor * kTapsPerPhase;  // 48

[[nodiscard]] std::vector<double> buildOversampleTaps() {
    std::vector<double> taps(static_cast<std::size_t>(kProtoLength), 0.0);
    const double center = (kProtoLength - 1) / 2.0;
    constexpr double fc = 0.125;  // cutoff = original Nyquist in the 4x domain
    double sum = 0.0;
    for (int n = 0; n < kProtoLength; ++n) {
        const double x = static_cast<double>(n) - center;
        const double arg = 2.0 * std::numbers::pi * fc * x;
        const double sinc =
            (std::abs(x) < 1e-12) ? 1.0 : std::sin(arg) / arg;
        const double lp = 2.0 * fc * sinc;
        const double window =
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) /
                                 (kProtoLength - 1));
        taps[static_cast<std::size_t>(n)] = lp * window;
        sum += lp * window;
    }
    // Normalise so each of the 4 polyphase branches has ~unity gain (total
    // gain = oversample factor), preserving signal amplitude.
    const double scale = (sum != 0.0) ? kOversampleFactor / sum : 1.0;
    for (double& t : taps) {
        t *= scale;
    }
    return taps;
}

}  // namespace

core::Result<LoudnessMeter> LoudnessMeter::create(const AudioFormat& format) {
    auto filter = KWeightingFilter::create(format);
    if (!filter.hasValue()) {
        // Propagate the 48 kHz precondition (and any future rate check).
        return filter.error();
    }
    return LoudnessMeter{format, std::move(filter).value()};
}

LoudnessMeter::LoudnessMeter(const AudioFormat& format, KWeightingFilter filter)
    : format_(format),
      filter_(std::move(filter)),
      channelWeights_(format.channelCount(), 1.0),
      framesPerSubBlock_(format.sampleRateHz() / 10),
      currentSumSq_(format.channelCount(), 0.0),
      truePeakHistory_(
          format.channelCount(),
          std::vector<double>(static_cast<std::size_t>(kTapsPerPhase), 0.0)),
      oversampleTaps_(buildOversampleTaps()) {
    for (std::uint32_t ch = 0; ch < format.channelCount(); ++ch) {
        channelWeights_[ch] = channelWeight(ch);
    }
}

core::Result<void> LoudnessMeter::addBlock(const AudioBuffer& buffer) {
    if (buffer.format() != format_) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "LoudnessMeter block format mismatch"};
    }
    if (buffer.empty()) {
        return core::ok();
    }

    const std::uint32_t channels = format_.channelCount();
    const std::size_t frames = buffer.frameCount();
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            const float raw = buffer.sample(frame, ch);
            if (!std::isfinite(raw)) {
                return core::AppError{
                    core::ErrorCode::InvalidArgument,
                    "LoudnessMeter received a non-finite sample"};
            }
            const double value = static_cast<double>(raw);

            // --- true peak: oversample the raw signal ---------------------
            std::vector<double>& hist = truePeakHistory_[ch];
            for (std::size_t k = hist.size() - 1; k > 0; --k) {
                hist[k] = hist[k - 1];
            }
            hist[0] = value;
            maxTruePeak_ = std::max(maxTruePeak_, std::abs(value));
            for (int phase = 0; phase < kOversampleFactor; ++phase) {
                double acc = 0.0;
                for (int k = 0; k < kTapsPerPhase; ++k) {
                    acc += oversampleTaps_[static_cast<std::size_t>(
                               kOversampleFactor * k + phase)] *
                           hist[static_cast<std::size_t>(k)];
                }
                maxTruePeak_ = std::max(maxTruePeak_, std::abs(acc));
            }

            // --- K-weighted mean square -----------------------------------
            const double weighted = static_cast<double>(
                filter_.processSample(ch, raw));
            currentSumSq_[ch] += weighted * weighted;
        }
        sawAnySample_ = true;
        if (++currentSubBlockFrames_ == framesPerSubBlock_) {
            finishSubBlock();
        }
    }
    return core::ok();
}

void LoudnessMeter::finishSubBlock() {
    recentSubBlocks_.push_back(currentSumSq_);
    while (recentSubBlocks_.size() > kRetainedSubBlocks) {
        recentSubBlocks_.pop_front();
    }

    // A completed 400 ms gating block is the last 4 sub-blocks (75% overlap:
    // one new gating block per 100 ms hop once 400 ms of audio exists).
    if (recentSubBlocks_.size() >= kSubBlocksPerMomentary) {
        const std::uint32_t channels = format_.channelCount();
        const double windowFrames =
            static_cast<double>(kSubBlocksPerMomentary * framesPerSubBlock_);
        double power = 0.0;
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            double sumSq = 0.0;
            for (std::size_t i = recentSubBlocks_.size() - kSubBlocksPerMomentary;
                 i < recentSubBlocks_.size(); ++i) {
                sumSq += recentSubBlocks_[i][ch];
            }
            const double meanSquare = sumSq / windowFrames;
            power += channelWeights_[ch] * meanSquare;
        }
        gatingBlockPowers_.push_back(power);
    }

    std::fill(currentSumSq_.begin(), currentSumSq_.end(), 0.0);
    currentSubBlockFrames_ = 0;
}

double LoudnessMeter::loudnessFromMeanSquares(
    const std::vector<double>& perChannelMeanSquare) const {
    double power = 0.0;
    for (std::uint32_t ch = 0; ch < perChannelMeanSquare.size(); ++ch) {
        power += channelWeights_[ch] * perChannelMeanSquare[ch];
    }
    if (power <= 0.0) {
        return kNoMeasurement;
    }
    return kLoudnessOffset + 10.0 * std::log10(power);
}

double LoudnessMeter::slidingWindowLoudness(std::size_t subBlockCount) const {
    if (recentSubBlocks_.size() < subBlockCount) {
        return kNoMeasurement;
    }
    const std::uint32_t channels = format_.channelCount();
    const double windowFrames =
        static_cast<double>(subBlockCount * framesPerSubBlock_);
    std::vector<double> meanSquare(channels, 0.0);
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        double sumSq = 0.0;
        for (std::size_t i = recentSubBlocks_.size() - subBlockCount;
             i < recentSubBlocks_.size(); ++i) {
            sumSq += recentSubBlocks_[i][ch];
        }
        meanSquare[ch] = sumSq / windowFrames;
    }
    return loudnessFromMeanSquares(meanSquare);
}

double LoudnessMeter::momentaryLufs() const {
    return slidingWindowLoudness(kSubBlocksPerMomentary);
}

double LoudnessMeter::shortTermLufs() const {
    return slidingWindowLoudness(kSubBlocksPerShortTerm);
}

double LoudnessMeter::integratedLufs() const {
    if (gatingBlockPowers_.empty()) {
        return kNoMeasurement;
    }

    // Absolute gate: block loudness >= -70 LKFS  <=>  power >= P_abs.
    const double absPower =
        std::pow(10.0, (kAbsoluteGateLkfs - kLoudnessOffset) / 10.0);
    double absSum = 0.0;
    std::size_t absCount = 0;
    for (const double power : gatingBlockPowers_) {
        if (power >= absPower) {
            absSum += power;
            ++absCount;
        }
    }
    if (absCount == 0) {
        return kNoMeasurement;
    }

    // Relative gate: threshold = mean(absolute-gated power) - 10 LU (a factor
    // of 10^(-1) in power). Blocks must clear both gates.
    const double meanAbs = absSum / static_cast<double>(absCount);
    const double relPower = meanAbs * std::pow(10.0, kRelativeGateLu / 10.0);
    const double threshold = std::max(absPower, relPower);

    double gatedSum = 0.0;
    std::size_t gatedCount = 0;
    for (const double power : gatingBlockPowers_) {
        if (power >= threshold) {
            gatedSum += power;
            ++gatedCount;
        }
    }
    if (gatedCount == 0) {
        return kNoMeasurement;
    }
    const double meanGated = gatedSum / static_cast<double>(gatedCount);
    return kLoudnessOffset + 10.0 * std::log10(meanGated);
}

double LoudnessMeter::truePeakDbtp() const {
    if (!sawAnySample_ || maxTruePeak_ <= 0.0) {
        return kNoMeasurement;
    }
    return 20.0 * std::log10(maxTruePeak_);
}

void LoudnessMeter::reset() {
    filter_.reset();
    std::fill(currentSumSq_.begin(), currentSumSq_.end(), 0.0);
    currentSubBlockFrames_ = 0;
    recentSubBlocks_.clear();
    gatingBlockPowers_.clear();
    for (std::vector<double>& hist : truePeakHistory_) {
        std::fill(hist.begin(), hist.end(), 0.0);
    }
    maxTruePeak_ = 0.0;
    sawAnySample_ = false;
}

}  // namespace creator::audio_dsp
