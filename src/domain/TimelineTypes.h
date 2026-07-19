#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace creator::domain {

[[nodiscard]] bool isValidUtf8(std::string_view value) noexcept;

class TimelineMarker final {
public:
    [[nodiscard]] static core::Result<TimelineMarker> create(
        MarkerId id, core::TimestampNs position, std::string label);

    [[nodiscard]] const MarkerId& id() const noexcept { return id_; }
    [[nodiscard]] core::TimestampNs position() const noexcept { return position_; }
    [[nodiscard]] const std::string& label() const noexcept { return label_; }

    friend bool operator==(const TimelineMarker&, const TimelineMarker&) = default;

private:
    TimelineMarker(MarkerId id, core::TimestampNs position, std::string label)
        : id_(std::move(id)), position_(position), label_(std::move(label)) {}

    MarkerId id_;
    core::TimestampNs position_;
    std::string label_;
};

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

enum class PipPreset {
    FullFrame,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Custom,
};

[[nodiscard]] core::Result<VisualTransform> visualTransformForPipPreset(
    PipPreset preset, double sourceAspect, double canvasAspect,
    std::int32_t zOrder);

[[nodiscard]] PipPreset identifyPipPreset(
    const VisualTransform& transform, double sourceAspect,
    double canvasAspect) noexcept;

enum class TextAlignment { Left, Center, Right };

class RgbaColor final {
public:
    [[nodiscard]] static core::Result<RgbaColor> parse(std::string value);
    [[nodiscard]] static RgbaColor fromChannels(
        std::uint8_t red, std::uint8_t green, std::uint8_t blue,
        std::uint8_t alpha) noexcept {
        return RgbaColor{red, green, blue, alpha};
    }

    [[nodiscard]] std::uint8_t red() const noexcept { return red_; }
    [[nodiscard]] std::uint8_t green() const noexcept { return green_; }
    [[nodiscard]] std::uint8_t blue() const noexcept { return blue_; }
    [[nodiscard]] std::uint8_t alpha() const noexcept { return alpha_; }
    [[nodiscard]] std::string toString() const;

    friend bool operator==(const RgbaColor&, const RgbaColor&) = default;

private:
    RgbaColor(std::uint8_t red, std::uint8_t green, std::uint8_t blue,
              std::uint8_t alpha) noexcept
        : red_(red), green_(green), blue_(blue), alpha_(alpha) {}

    std::uint8_t red_;
    std::uint8_t green_;
    std::uint8_t blue_;
    std::uint8_t alpha_;
};

class TitlePayload final {
public:
    [[nodiscard]] static core::Result<TitlePayload> create(
        std::string text, std::string fontFamily, double x, double y,
        RgbaColor foreground, RgbaColor background,
        TextAlignment alignment);

    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] const std::string& fontFamily() const noexcept {
        return fontFamily_;
    }
    [[nodiscard]] double x() const noexcept { return x_; }
    [[nodiscard]] double y() const noexcept { return y_; }
    [[nodiscard]] const RgbaColor& foreground() const noexcept {
        return foreground_;
    }
    [[nodiscard]] const RgbaColor& background() const noexcept {
        return background_;
    }
    [[nodiscard]] TextAlignment alignment() const noexcept { return alignment_; }

    friend bool operator==(const TitlePayload&, const TitlePayload&) = default;

private:
    TitlePayload(std::string text, std::string fontFamily, double x, double y,
                 RgbaColor foreground, RgbaColor background,
                 TextAlignment alignment)
        : text_(std::move(text)),
          fontFamily_(std::move(fontFamily)),
          x_(x),
          y_(y),
          foreground_(foreground),
          background_(background),
          alignment_(alignment) {}

    std::string text_;
    std::string fontFamily_;
    double x_;
    double y_;
    RgbaColor foreground_;
    RgbaColor background_;
    TextAlignment alignment_;
};

class CaptionCue final {
public:
    [[nodiscard]] static core::Result<CaptionCue> create(
        CueId id, core::DurationNs startOffset, core::DurationNs duration,
        std::string text);

    [[nodiscard]] const CueId& id() const noexcept { return id_; }
    [[nodiscard]] core::DurationNs startOffset() const noexcept {
        return startOffset_;
    }
    [[nodiscard]] core::DurationNs duration() const noexcept { return duration_; }
    [[nodiscard]] core::DurationNs endOffset() const noexcept {
        return startOffset_ + duration_;
    }
    [[nodiscard]] const std::string& text() const noexcept { return text_; }

    friend bool operator==(const CaptionCue&, const CaptionCue&) = default;

private:
    CaptionCue(CueId id, core::DurationNs startOffset,
               core::DurationNs duration, std::string text)
        : id_(std::move(id)),
          startOffset_(startOffset),
          duration_(duration),
          text_(std::move(text)) {}

    CueId id_;
    core::DurationNs startOffset_;
    core::DurationNs duration_;
    std::string text_;
};

}  // namespace creator::domain
