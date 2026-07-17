#include "domain/TimelineTypes.h"

#include "core/AppError.h"

#include <cmath>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <string_view>

namespace creator::domain {
namespace {

using core::AppError;
using core::ErrorCode;

bool normalized(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

std::optional<std::size_t> utf8CodePointCount(std::string_view value) noexcept {
    std::size_t count = 0;
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::size_t width = 0;
        std::uint32_t codePoint = 0;
        if (first <= 0x7fU) {
            width = 1;
            codePoint = first;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            width = 2;
            codePoint = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            width = 3;
            codePoint = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            width = 4;
            codePoint = first & 0x07U;
        } else {
            return std::nullopt;
        }
        if (width > value.size() - index) return std::nullopt;
        for (std::size_t offset = 1; offset < width; ++offset) {
            const auto continuation =
                static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) return std::nullopt;
            codePoint = (codePoint << 6U) | (continuation & 0x3fU);
        }
        const bool overlong =
            (width == 2 && codePoint < 0x80U) ||
            (width == 3 && codePoint < 0x800U) ||
            (width == 4 && codePoint < 0x10000U);
        if (overlong || (codePoint >= 0xd800U && codePoint <= 0xdfffU) ||
            codePoint > 0x10ffffU || codePoint == 0) {
            return std::nullopt;
        }
        index += width;
        ++count;
    }
    return count;
}

std::optional<std::uint8_t> hexByte(char high, char low) noexcept {
    const auto nibble = [](char value) -> std::optional<std::uint8_t> {
        if (value >= '0' && value <= '9') {
            return static_cast<std::uint8_t>(value - '0');
        }
        if (value >= 'a' && value <= 'f') {
            return static_cast<std::uint8_t>(value - 'a' + 10);
        }
        return std::nullopt;
    };
    const auto upper = nibble(high);
    const auto lower = nibble(low);
    if (!upper.has_value() || !lower.has_value()) return std::nullopt;
    return static_cast<std::uint8_t>((*upper << 4U) | *lower);
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

core::Result<VisualTransform> visualTransformForPipPreset(
    PipPreset preset, double sourceAspect, double canvasAspect,
    std::int32_t zOrder) {
    if (!std::isfinite(sourceAspect) || sourceAspect <= 0.0 ||
        !std::isfinite(canvasAspect) || canvasAspect <= 0.0 ||
        preset == PipPreset::Custom) {
        return AppError{ErrorCode::InvalidArgument,
                        "PIP preset inputs are outside valid bounds"};
    }
    if (preset == PipPreset::FullFrame) {
        return VisualTransform::create(
            0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 0.0, 1.0, zOrder);
    }

    constexpr double kWidth = 0.30;
    constexpr double kMargin = 0.04;
    const double height = kWidth * canvasAspect / sourceAspect;
    if (!std::isfinite(height) || height <= 0.0 ||
        height > 1.0 - (2.0 * kMargin)) {
        return AppError{ErrorCode::InvalidArgument,
                        "PIP source aspect does not fit the safe area"};
    }
    const bool right = preset == PipPreset::TopRight ||
                       preset == PipPreset::BottomRight;
    const bool bottom = preset == PipPreset::BottomLeft ||
                        preset == PipPreset::BottomRight;
    const double x = right ? 1.0 - kMargin - kWidth : kMargin;
    const double y = bottom ? 1.0 - kMargin - height : kMargin;
    return VisualTransform::create(
        x, y, kWidth, height, 1.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 0.0, 1.0, zOrder);
}

PipPreset identifyPipPreset(const VisualTransform& transform,
                            double sourceAspect,
                            double canvasAspect) noexcept {
    constexpr std::array presets{
        PipPreset::FullFrame, PipPreset::TopLeft, PipPreset::TopRight,
        PipPreset::BottomLeft, PipPreset::BottomRight};
    for (const auto preset : presets) {
        const auto candidate = visualTransformForPipPreset(
            preset, sourceAspect, canvasAspect, transform.zOrder());
        if (candidate.hasValue() && candidate.value() == transform) return preset;
    }
    return PipPreset::Custom;
}

core::Result<RgbaColor> RgbaColor::parse(std::string value) {
    if (value.size() != 9 || value.front() != '#') {
        return AppError{ErrorCode::InvalidArgument,
                        "RGBA color must use canonical #rrggbbaa form"};
    }
    const auto red = hexByte(value[1], value[2]);
    const auto green = hexByte(value[3], value[4]);
    const auto blue = hexByte(value[5], value[6]);
    const auto alpha = hexByte(value[7], value[8]);
    if (!red.has_value() || !green.has_value() || !blue.has_value() ||
        !alpha.has_value()) {
        return AppError{ErrorCode::InvalidArgument,
                        "RGBA color must use canonical lowercase hexadecimal"};
    }
    return RgbaColor{*red, *green, *blue, *alpha};
}

std::string RgbaColor::toString() const {
    constexpr char digits[] = "0123456789abcdef";
    const std::array values{red_, green_, blue_, alpha_};
    std::string result(9, '#');
    result[0] = '#';
    std::size_t index = 1;
    for (const auto value : values) {
        result[index++] = digits[value >> 4U];
        result[index++] = digits[value & 0x0fU];
    }
    return result;
}

core::Result<TitlePayload> TitlePayload::create(
    std::string text, std::string fontFamily, double x, double y,
    RgbaColor foreground, RgbaColor background, TextAlignment alignment) {
    const auto textLength = utf8CodePointCount(text);
    const auto familyLength = utf8CodePointCount(fontFamily);
    const bool knownAlignment = alignment == TextAlignment::Left ||
                                alignment == TextAlignment::Center ||
                                alignment == TextAlignment::Right;
    if (!textLength.has_value() || *textLength == 0 || *textLength > 512 ||
        !familyLength.has_value() || *familyLength == 0 ||
        *familyLength > 128 || !normalized(x) || !normalized(y) ||
        !knownAlignment) {
        return AppError{ErrorCode::InvalidArgument,
                        "title payload is outside valid bounds"};
    }
    return TitlePayload{std::move(text), std::move(fontFamily), x, y,
                        foreground, background, alignment};
}

core::Result<CaptionCue> CaptionCue::create(
    CueId id, core::DurationNs startOffset, core::DurationNs duration,
    std::string text) {
    const auto start = startOffset.count();
    const auto span = duration.count();
    const auto length = utf8CodePointCount(text);
    if (start < 0 || span <= 0 ||
        start > std::numeric_limits<std::int64_t>::max() - span ||
        !length.has_value() || *length == 0 || *length > 2000) {
        return AppError{ErrorCode::InvalidArgument,
                        "caption cue is outside valid bounds"};
    }
    return CaptionCue{std::move(id), startOffset, duration, std::move(text)};
}

}  // namespace creator::domain
