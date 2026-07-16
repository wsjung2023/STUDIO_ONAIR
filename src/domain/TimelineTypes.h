#pragma once

#include "core/Result.h"
#include "core/Timebase.h"

#include <cstdint>

namespace creator::domain {

class TimeRange final {
public:
    [[nodiscard]] static core::Result<TimeRange> create(
        core::TimestampNs start, core::DurationNs duration);

    [[nodiscard]] core::TimestampNs start() const noexcept { return start_; }
    [[nodiscard]] core::DurationNs duration() const noexcept { return duration_; }
    [[nodiscard]] core::TimestampNs end() const noexcept { return start_ + duration_; }

    friend bool operator==(const TimeRange&, const TimeRange&) = default;

private:
    TimeRange(core::TimestampNs start, core::DurationNs duration)
        : start_(start), duration_(duration) {}

    core::TimestampNs start_;
    core::DurationNs duration_;
};

[[nodiscard]] bool overlaps(const TimeRange& first, const TimeRange& second) noexcept;

class VisualTransform final {
public:
    [[nodiscard]] static core::Result<VisualTransform> create(
        double x, double y, double width, double height,
        double scaleX, double scaleY, double rotationDegrees,
        double cropLeft, double cropTop, double cropRight, double cropBottom,
        double opacity, std::int32_t zOrder);

    [[nodiscard]] double x() const noexcept { return x_; }
    [[nodiscard]] double y() const noexcept { return y_; }
    [[nodiscard]] double width() const noexcept { return width_; }
    [[nodiscard]] double height() const noexcept { return height_; }
    [[nodiscard]] double scaleX() const noexcept { return scaleX_; }
    [[nodiscard]] double scaleY() const noexcept { return scaleY_; }
    [[nodiscard]] double rotationDegrees() const noexcept { return rotationDegrees_; }
    [[nodiscard]] double cropLeft() const noexcept { return cropLeft_; }
    [[nodiscard]] double cropTop() const noexcept { return cropTop_; }
    [[nodiscard]] double cropRight() const noexcept { return cropRight_; }
    [[nodiscard]] double cropBottom() const noexcept { return cropBottom_; }
    [[nodiscard]] double opacity() const noexcept { return opacity_; }
    [[nodiscard]] std::int32_t zOrder() const noexcept { return zOrder_; }

    friend bool operator==(const VisualTransform&, const VisualTransform&) = default;

private:
    VisualTransform(double x, double y, double width, double height,
                    double scaleX, double scaleY, double rotationDegrees,
                    double cropLeft, double cropTop, double cropRight,
                    double cropBottom, double opacity, std::int32_t zOrder)
        : x_(x),
          y_(y),
          width_(width),
          height_(height),
          scaleX_(scaleX),
          scaleY_(scaleY),
          rotationDegrees_(rotationDegrees),
          cropLeft_(cropLeft),
          cropTop_(cropTop),
          cropRight_(cropRight),
          cropBottom_(cropBottom),
          opacity_(opacity),
          zOrder_(zOrder) {}

    double x_;
    double y_;
    double width_;
    double height_;
    double scaleX_;
    double scaleY_;
    double rotationDegrees_;
    double cropLeft_;
    double cropTop_;
    double cropRight_;
    double cropBottom_;
    double opacity_;
    std::int32_t zOrder_;
};

class AudioEnvelope final {
public:
    [[nodiscard]] static core::Result<AudioEnvelope> create(
        double gainDb, core::DurationNs fadeIn, core::DurationNs fadeOut,
        core::DurationNs clipDuration);

    [[nodiscard]] double gainDb() const noexcept { return gainDb_; }
    [[nodiscard]] core::DurationNs fadeIn() const noexcept { return fadeIn_; }
    [[nodiscard]] core::DurationNs fadeOut() const noexcept { return fadeOut_; }

    friend bool operator==(const AudioEnvelope&, const AudioEnvelope&) = default;

private:
    AudioEnvelope(double gainDb, core::DurationNs fadeIn, core::DurationNs fadeOut)
        : gainDb_(gainDb), fadeIn_(fadeIn), fadeOut_(fadeOut) {}

    double gainDb_;
    core::DurationNs fadeIn_;
    core::DurationNs fadeOut_;
};

}  // namespace creator::domain
