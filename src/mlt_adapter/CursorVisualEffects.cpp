#include "mlt_adapter/CursorVisualEffects.h"

#include "cursor/CursorButton.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <utility>

namespace creator::mlt_adapter {
namespace {

core::AppError invalid(std::string message) {
    return {core::ErrorCode::InvalidArgument, std::move(message)};
}

struct ActiveZoom final {
    const autozoom::ZoomRegion* region{};
};

std::optional<ActiveZoom> activeZoom(core::TimestampNs position,
                                     const CursorVisualEffectsPlan& plan) {
    for (const auto& candidate : plan.zooms) {
        if (position >= candidate.span().start() &&
            position < candidate.span().end()) {
            return ActiveZoom{&candidate.region()};
        }
    }
    return std::nullopt;
}

bool active(core::TimestampNs position,
            const cursor_emphasis::ClickEmphasis& emphasis) noexcept {
    return position >= emphasis.startNs() && position < emphasis.endNs();
}

std::array<std::uint8_t, 3> color(cursor::CursorButton button) noexcept {
    switch (button) {
        case cursor::CursorButton::Left:
            return {80U, 100U, 255U};  // BGRA: warm red
        case cursor::CursorButton::Right:
            return {255U, 100U, 80U};  // cool blue
        case cursor::CursorButton::Middle:
            return {80U, 220U, 100U};
    }
    return {255U, 255U, 255U};
}

void blendPixel(std::uint8_t* pixel, std::array<std::uint8_t, 3> rgb,
                double alpha) noexcept {
    alpha = std::clamp(alpha, 0.0, 1.0);
    const double sourceAlpha = static_cast<double>(pixel[3]) / 255.0;
    const double outputAlpha = alpha + sourceAlpha * (1.0 - alpha);
    if (outputAlpha <= 0.0) return;
    for (std::size_t channel = 0; channel < 3U; ++channel) {
        const double output =
            (static_cast<double>(rgb[channel]) * alpha +
             static_cast<double>(pixel[channel]) * sourceAlpha * (1.0 - alpha)) /
            outputAlpha;
        pixel[channel] = static_cast<std::uint8_t>(
            std::clamp(std::lround(output), 0L, 255L));
    }
    pixel[3] = static_cast<std::uint8_t>(
        std::clamp(std::lround(outputAlpha * 255.0), 0L, 255L));
}

void drawEmphasis(ProcessedBgraFrame& frame,
                  const cursor_emphasis::ClickEmphasis& emphasis,
                  core::TimestampNs position,
                  const std::optional<ActiveZoom>& zoom) {
    const auto elapsed = (position - emphasis.startNs()).count();
    const auto duration = emphasis.duration().count();
    const double progress = std::clamp(
        static_cast<double>(elapsed) / static_cast<double>(duration), 0.0, 1.0);
    double x = emphasis.position().x();
    double y = emphasis.position().y();
    if (zoom.has_value()) {
        x = (x - zoom->region->visibleX()) / zoom->region->visibleWidth();
        y = (y - zoom->region->visibleY()) / zoom->region->visibleHeight();
    }
    if (x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0) return;
    const double minDimension = static_cast<double>(
        std::min(frame.width(), frame.height()));
    const double baseRadius = emphasis.radius() * minDimension;
    const double radius = emphasis.style() == cursor_emphasis::EmphasisStyle::Ripple
                              ? baseRadius * (0.25 + progress * 0.75)
                              : baseRadius;
    const double opacity = emphasis.style() == cursor_emphasis::EmphasisStyle::Ripple
                               ? (1.0 - progress) * 0.9
                               : 0.8;
    const double thickness = std::max(1.0, baseRadius * 0.08);
    const auto centerX = x * static_cast<double>(frame.width() - 1U);
    const auto centerY = y * static_cast<double>(frame.height() - 1U);
    const auto minX = static_cast<std::uint32_t>(std::max(0.0, centerX - radius - thickness));
    const auto maxX = static_cast<std::uint32_t>(std::min(
        static_cast<double>(frame.width() - 1U), centerX + radius + thickness));
    const auto minY = static_cast<std::uint32_t>(std::max(0.0, centerY - radius - thickness));
    const auto maxY = static_cast<std::uint32_t>(std::min(
        static_cast<double>(frame.height() - 1U), centerY + radius + thickness));
    const auto ringColor = color(emphasis.button());
    auto bytes = frame.mutableBytes();
    for (std::uint32_t py = minY; py <= maxY; ++py) {
        for (std::uint32_t px = minX; px <= maxX; ++px) {
            const double dx = static_cast<double>(px) - centerX;
            const double dy = static_cast<double>(py) - centerY;
            const double distance = std::sqrt(dx * dx + dy * dy);
            const double distanceToRing = std::abs(distance - radius);
            if (distanceToRing > thickness) continue;
            const double edge = 1.0 - distanceToRing / thickness;
            auto* pixel = bytes.data() + static_cast<std::size_t>(py) * frame.stride() +
                          static_cast<std::size_t>(px) * 4U;
            blendPixel(pixel, ringColor, opacity * edge);
        }
    }
}

}  // namespace

core::Result<ProcessedBgraFrame> applyCursorVisualEffects(
    BgraFrameView source, std::uint32_t canvasWidth, std::uint32_t canvasHeight,
    core::TimestampNs position, const CursorVisualEffectsPlan& plan) {
    const auto zoom = activeZoom(position, plan);
    std::optional<domain::VisualTransform> transform;
    if (zoom.has_value()) {
        auto created = domain::VisualTransform::create(
            zoom->region->visibleX(), zoom->region->visibleY(),
            zoom->region->visibleWidth(), zoom->region->visibleHeight(),
            zoom->region->zoomFactor(), zoom->region->zoomFactor(), 0.0,
            0.0, 0.0, 0.0, 0.0, 1.0, 0);
        if (!created.hasValue()) return created.error();
        transform = created.value();
    }

    const bool hasClick = std::any_of(
        plan.clicks.begin(), plan.clicks.end(),
        [position](const auto& emphasis) { return active(position, emphasis); });
    if (!zoom.has_value() && !hasClick) {
        auto identity = domain::VisualTransform::create(
            0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0);
        if (!identity.hasValue()) return identity.error();
        return applyVisualTransform(source, canvasWidth, canvasHeight,
                                    identity.value());
    }

    auto identity = domain::VisualTransform::create(
        0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0);
    if (!identity.hasValue()) return identity.error();
    const auto& selectedTransform = transform.has_value() ? *transform : identity.value();
    auto transformed = applyVisualTransform(
        source, canvasWidth, canvasHeight, selectedTransform);
    if (!transformed.hasValue()) return transformed.error();
    if (transformed.value().aliasesInput()) {
        const auto bytes = transformed.value().bytes();
        std::vector<std::uint8_t> owned{bytes.begin(), bytes.end()};
        transformed = ProcessedBgraFrame::own(
            std::move(owned), transformed.value().width(),
            transformed.value().height(), transformed.value().stride());
    }
    for (const auto& emphasis : plan.clicks) {
        if (active(position, emphasis)) drawEmphasis(transformed.value(), emphasis,
                                                     position, zoom);
    }
    return transformed;
}

}  // namespace creator::mlt_adapter
