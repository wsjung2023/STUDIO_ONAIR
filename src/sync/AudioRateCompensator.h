#pragma once

#include "core/Result.h"

#include <cstdint>

namespace creator::synchronization {

// Converts bounded fractional sample-rate corrections into whole-sample
// compensation without discarding sub-sample corrections between blocks.
class AudioRateCompensator final {
public:
    [[nodiscard]] core::Result<int> next(std::uint32_t frameCount,
                                         double sampleRateRatio) noexcept;
    [[nodiscard]] double pendingSamples() const noexcept;

private:
    double pendingSamples_{0.0};
};

}  // namespace creator::synchronization
