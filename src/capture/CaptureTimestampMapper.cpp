#include "capture/CaptureTimestampMapper.h"

#include "core/AppError.h"

#include <cstdint>
#include <limits>

namespace creator::capture {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;

Result<core::TimestampNs> invalidTimestamp(const char* message) {
    return AppError{ErrorCode::InvalidArgument, message};
}

Result<std::uint64_t> ticksToNanoseconds(std::uint64_t ticks, std::uint32_t timescale) {
    const auto scale = static_cast<std::uint64_t>(timescale);
    const auto wholeSeconds = ticks / scale;
    const auto remainderTicks = ticks % scale;

    if (wholeSeconds > std::numeric_limits<std::uint64_t>::max() /
                           kNanosecondsPerSecond) {
        return AppError{ErrorCode::InvalidArgument,
                        "native timestamp delta exceeds the project timeline"};
    }

    const auto wholeNanoseconds = wholeSeconds * kNanosecondsPerSecond;
    // timescale is a positive int32, so remainderTicks * 1e9 is at most
    // 2.147e18 and cannot overflow uint64.
    const auto fractionalNanoseconds =
        (remainderTicks * kNanosecondsPerSecond) / scale;
    if (fractionalNanoseconds >
        std::numeric_limits<std::uint64_t>::max() - wholeNanoseconds) {
        return AppError{ErrorCode::InvalidArgument,
                        "native timestamp delta exceeds the project timeline"};
    }
    return wholeNanoseconds + fractionalNanoseconds;
}

Result<core::TimestampNs> addChecked(core::TimestampNs anchor, std::uint64_t delta) {
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
    const auto anchorCount = anchor.time_since_epoch().count();

    const std::uint64_t capacity = anchorCount >= 0
                                       ? static_cast<std::uint64_t>(kMax - anchorCount)
                                       : static_cast<std::uint64_t>(kMax) +
                                             static_cast<std::uint64_t>(-(anchorCount + 1)) + 1;
    if (delta > capacity) {
        return invalidTimestamp("mapped timestamp exceeds the project timeline");
    }

    std::int64_t mappedCount = 0;
    if (delta <= static_cast<std::uint64_t>(kMax)) {
        mappedCount = anchorCount + static_cast<std::int64_t>(delta);
    } else {
        // A delta greater than INT64_MAX can fit only when anchorCount is
        // negative. Subtract its magnitude in unsigned space before converting.
        const auto anchorMagnitude =
            static_cast<std::uint64_t>(-(anchorCount + 1)) + 1;
        mappedCount = static_cast<std::int64_t>(delta - anchorMagnitude);
    }
    return core::TimestampNs{core::ProjectClock::duration{mappedCount}};
}

}  // namespace

CaptureTimestampMapper::CaptureTimestampMapper(core::TimestampNs projectAnchor) noexcept
    : projectAnchor_(projectAnchor) {}

Result<core::TimestampNs> CaptureTimestampMapper::map(NativeTimestamp timestamp) {
    if (timestamp.timescale <= 0) {
        return invalidTimestamp("native timestamp timescale must be positive");
    }

    if (!nativeAnchor_) {
        nativeAnchor_ = timestamp;
        lastNativeValue_ = timestamp.value;
        return projectAnchor_;
    }

    if (timestamp.timescale != nativeAnchor_->timescale) {
        return invalidTimestamp("native timestamp timescale changed within one stream");
    }
    if (timestamp.value < *lastNativeValue_) {
        return invalidTimestamp("native timestamp moved backward");
    }

    // Unsigned subtraction gives the exact non-negative mathematical delta for
    // any ordered pair of int64 values, including INT64_MIN to INT64_MAX.
    const auto deltaTicks = static_cast<std::uint64_t>(timestamp.value) -
                            static_cast<std::uint64_t>(nativeAnchor_->value);
    auto deltaNanoseconds =
        ticksToNanoseconds(deltaTicks, static_cast<std::uint32_t>(timestamp.timescale));
    if (!deltaNanoseconds.hasValue()) {
        return deltaNanoseconds.error();
    }

    auto mapped = addChecked(projectAnchor_, deltaNanoseconds.value());
    if (mapped.hasValue()) {
        lastNativeValue_ = timestamp.value;
    }
    return mapped;
}

void CaptureTimestampMapper::reset(core::TimestampNs projectAnchor) noexcept {
    projectAnchor_ = projectAnchor;
    nativeAnchor_.reset();
    lastNativeValue_.reset();
}

}  // namespace creator::capture

