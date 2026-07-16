#pragma once

#include "core/Result.h"
#include "media/MediaTypes.h"

namespace creator::capture {

struct AudioLevel final {
    double peakLinear{0.0};
    double rmsLinear{0.0};
    double peakDbfs{-96.0};
    double rmsDbfs{-96.0};

    friend bool operator==(const AudioLevel&, const AudioLevel&) = default;
};

/// Validates one neutral PCM block and calculates whole-block peak and RMS.
class AudioLevelMeter final {
public:
    [[nodiscard]] static creator::core::Result<AudioLevel> measure(
        const creator::media::AudioBlock& block);
};

}  // namespace creator::capture
