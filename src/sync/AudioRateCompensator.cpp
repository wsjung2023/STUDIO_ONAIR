#include "sync/AudioRateCompensator.h"

#include "core/AppError.h"

#include <cmath>
#include <limits>

namespace creator::synchronization {
namespace {

constexpr double minimumRateRatio = 0.999;
constexpr double maximumRateRatio = 1.001;

}  // namespace

core::Result<int> AudioRateCompensator::next(
    std::uint32_t frameCount, double sampleRateRatio) noexcept {
    if (frameCount == 0 || !std::isfinite(sampleRateRatio) ||
        sampleRateRatio < minimumRateRatio ||
        sampleRateRatio > maximumRateRatio) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Audio rate correction exceeds the safe 1000 ppm bound"};
    }

    const double nextPending = pendingSamples_ +
        static_cast<double>(frameCount) * (sampleRateRatio - 1.0);
    if (!std::isfinite(nextPending) ||
        nextPending < static_cast<double>(std::numeric_limits<int>::min()) ||
        nextPending > static_cast<double>(std::numeric_limits<int>::max())) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Audio rate correction accumulator overflowed"};
    }

    const int wholeSamples = static_cast<int>(std::trunc(nextPending));
    pendingSamples_ = nextPending - static_cast<double>(wholeSamples);
    return wholeSamples;
}

double AudioRateCompensator::pendingSamples() const noexcept {
    return pendingSamples_;
}

}  // namespace creator::synchronization
