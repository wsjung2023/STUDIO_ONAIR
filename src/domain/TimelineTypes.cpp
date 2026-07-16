#include "domain/TimelineTypes.h"

#include "core/AppError.h"

#include <cmath>
#include <limits>

namespace creator::domain {
namespace {

using core::AppError;
using core::ErrorCode;

bool normalized(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

}  // namespace

core::Result<TimeRange> TimeRange::create(core::TimestampNs start,
                                          core::DurationNs duration) {
    const auto startCount = start.time_since_epoch().count();
    const auto durationCount = duration.count();
    if (startCount < 0 || durationCount <= 0) {
        return AppError{ErrorCode::InvalidArgument,
                        "time range must have a non-negative start and positive duration"};
    }
    if (startCount > std::numeric_limits<std::int64_t>::max() - durationCount) {
        return AppError{ErrorCode::InvalidArgument, "time range end exceeds project time"};
    }
    return TimeRange{start, duration};
}

bool overlaps(const TimeRange& first, const TimeRange& second) noexcept {
    return first.start() < second.end() && second.start() < first.end();
}

core::Result<VisualTransform> VisualTransform::create(
    double x, double y, double width, double height,
    double scaleX, double scaleY, double rotationDegrees,
    double cropLeft, double cropTop, double cropRight, double cropBottom,
    double opacity, std::int32_t zOrder) {
    if (!normalized(x) || !normalized(y) || !normalized(width) ||
        !normalized(height) || width <= 0.0 || height <= 0.0 ||
        !std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0.0 ||
        scaleY <= 0.0 || !std::isfinite(rotationDegrees) ||
        !normalized(cropLeft) || !normalized(cropTop) ||
        !normalized(cropRight) || !normalized(cropBottom) ||
        cropLeft + cropRight >= 1.0 || cropTop + cropBottom >= 1.0 ||
        !normalized(opacity)) {
        return AppError{ErrorCode::InvalidArgument, "visual transform is outside valid bounds"};
    }
    return VisualTransform{x, y, width, height, scaleX, scaleY, rotationDegrees,
                           cropLeft, cropTop, cropRight, cropBottom, opacity, zOrder};
}

core::Result<AudioEnvelope> AudioEnvelope::create(
    double gainDb, core::DurationNs fadeIn, core::DurationNs fadeOut,
    core::DurationNs clipDuration) {
    if (!std::isfinite(gainDb) || gainDb < -96.0 || gainDb > 24.0 ||
        fadeIn < core::DurationNs::zero() || fadeOut < core::DurationNs::zero() ||
        clipDuration <= core::DurationNs::zero() || fadeIn > clipDuration ||
        fadeOut > clipDuration - fadeIn) {
        return AppError{ErrorCode::InvalidArgument, "audio envelope is outside valid bounds"};
    }
    return AudioEnvelope{gainDb, fadeIn, fadeOut};
}

}  // namespace creator::domain
