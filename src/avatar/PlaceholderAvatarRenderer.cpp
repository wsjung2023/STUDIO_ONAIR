#include "avatar/PlaceholderAvatarRenderer.h"

#include "core/AppError.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace creator::avatar {
namespace {

struct Bgra final {
    std::uint8_t b;
    std::uint8_t g;
    std::uint8_t r;
    std::uint8_t a;
};

/// A tiny opaque software canvas. Deliberately independent of any graphics
/// library so the placeholder stays in the Qt-free avatar layer.
class Canvas final {
public:
    Canvas(std::uint32_t width, std::uint32_t height)
        : width_(width),
          height_(height),
          pixels_(static_cast<std::size_t>(width) * height * 4U, 0U) {}

    void fill(Bgra color) noexcept {
        for (std::size_t index = 0; index < pixels_.size(); index += 4U) {
            pixels_[index] = color.b;
            pixels_[index + 1U] = color.g;
            pixels_[index + 2U] = color.r;
            pixels_[index + 3U] = color.a;
        }
    }

    void setPixel(int x, int y, Bgra color) noexcept {
        if (x < 0 || y < 0 || x >= static_cast<int>(width_) ||
            y >= static_cast<int>(height_)) {
            return;
        }
        const auto offset =
            (static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)) * 4U;
        pixels_[offset] = color.b;
        pixels_[offset + 1U] = color.g;
        pixels_[offset + 2U] = color.r;
        pixels_[offset + 3U] = color.a;
    }

    void fillRect(int x0, int y0, int x1, int y1, Bgra color) noexcept {
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) setPixel(x, y, color);
        }
    }

    void fillEllipse(float cx, float cy, float rx, float ry, Bgra color) noexcept {
        if (rx < 0.5F || ry < 0.5F) return;
        const int minX = static_cast<int>(std::floor(cx - rx));
        const int maxX = static_cast<int>(std::ceil(cx + rx));
        const int minY = static_cast<int>(std::floor(cy - ry));
        const int maxY = static_cast<int>(std::ceil(cy + ry));
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                const float dx = (static_cast<float>(x) + 0.5F - cx) / rx;
                const float dy = (static_cast<float>(y) + 0.5F - cy) / ry;
                if (dx * dx + dy * dy <= 1.0F) setPixel(x, y, color);
            }
        }
    }

    [[nodiscard]] std::vector<std::uint8_t> take() noexcept {
        return std::move(pixels_);
    }

private:
    std::uint32_t width_;
    std::uint32_t height_;
    std::vector<std::uint8_t> pixels_;
};

float valueOf(std::span<const AvatarParameterValue> parameters,
              std::string_view name, float fallback) noexcept {
    for (const auto& parameter : parameters) {
        if (parameter.modelParameter == name) return parameter.value;
    }
    return fallback;
}

// Rotates the offset (dx, dy) by `angle` radians around the origin. Used to
// tilt facial features together so head-roll is visible.
void rotate(float& dx, float& dy, float angle) noexcept {
    const float cosA = std::cos(angle);
    const float sinA = std::sin(angle);
    const float rx = dx * cosA - dy * sinA;
    const float ry = dx * sinA + dy * cosA;
    dx = rx;
    dy = ry;
}

}  // namespace

std::vector<AvatarParameterBinding> placeholderAvatarBindings() {
    using Names = PlaceholderAvatarParameterNames;
    return {
        {Names::kEyeOpenLeft, AvatarParameterSource::EyeOpenLeft, 1.0F, 0.0F, 0.0F, 1.0F},
        {Names::kEyeOpenRight, AvatarParameterSource::EyeOpenRight, 1.0F, 0.0F, 0.0F, 1.0F},
        {Names::kBrowUpLeft, AvatarParameterSource::BrowUpLeft, 1.0F, 0.0F, 0.0F, 1.0F},
        {Names::kBrowUpRight, AvatarParameterSource::BrowUpRight, 1.0F, 0.0F, 0.0F, 1.0F},
        {Names::kMouthOpen, AvatarParameterSource::MouthOpen, 1.0F, 0.0F, 0.0F, 1.0F},
        {Names::kMouthWide, AvatarParameterSource::MouthWide, 1.0F, 0.0F, 0.0F, 1.0F},
        {Names::kHeadYaw, AvatarParameterSource::HeadYaw, 1.0F, 0.0F, -1.0F, 1.0F},
        {Names::kHeadPitch, AvatarParameterSource::HeadPitch, 1.0F, 0.0F, -1.0F, 1.0F},
        {Names::kHeadRoll, AvatarParameterSource::HeadRoll, 1.0F, 0.0F, -1.0F, 1.0F},
    };
}

core::Result<AvatarRenderFrame> PlaceholderAvatarRenderer::render(
    core::TimestampNs timestamp,
    std::span<const AvatarParameterValue> parameters) {
    if (width_ == 0U || height_ == 0U) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "placeholder avatar frame dimensions are empty"};
    }

    using Names = PlaceholderAvatarParameterNames;
    const float eyeOpenLeft = std::clamp(valueOf(parameters, Names::kEyeOpenLeft, 1.0F), 0.0F, 1.0F);
    const float eyeOpenRight = std::clamp(valueOf(parameters, Names::kEyeOpenRight, 1.0F), 0.0F, 1.0F);
    const float browUpLeft = std::clamp(valueOf(parameters, Names::kBrowUpLeft, 0.0F), 0.0F, 1.0F);
    const float browUpRight = std::clamp(valueOf(parameters, Names::kBrowUpRight, 0.0F), 0.0F, 1.0F);
    const float mouthOpen = std::clamp(valueOf(parameters, Names::kMouthOpen, 0.0F), 0.0F, 1.0F);
    const float mouthWide = std::clamp(valueOf(parameters, Names::kMouthWide, 0.0F), 0.0F, 1.0F);
    const float headYaw = std::clamp(valueOf(parameters, Names::kHeadYaw, 0.0F), -1.0F, 1.0F);
    const float headPitch = std::clamp(valueOf(parameters, Names::kHeadPitch, 0.0F), -1.0F, 1.0F);
    const float headRoll = std::clamp(valueOf(parameters, Names::kHeadRoll, 0.0F), -1.0F, 1.0F);

    const float width = static_cast<float>(width_);
    const float height = static_cast<float>(height_);

    Canvas canvas{width_, height_};
    // Opaque studio-blue backdrop so the recorded avatar track is never a flat
    // frame and reads clearly as a rendered source.
    canvas.fill(Bgra{60, 44, 30, 255});
    // Warning banner along the top marks this unmistakably as a synthetic
    // placeholder, independent of any UI overlay.
    canvas.fillRect(0, 0, static_cast<int>(width_) - 1,
                    static_cast<int>(height * 0.08F), Bgra{40, 140, 235, 255});

    const float roll = headRoll * 0.35F;  // radians
    const float headR = std::min(width, height) * 0.32F;
    const float cx = width * 0.5F + headYaw * width * 0.14F;
    const float cy = height * 0.55F - headPitch * height * 0.10F;

    // Head.
    canvas.fillEllipse(cx, cy, headR * 0.82F, headR, Bgra{150, 190, 235, 255});

    const auto placeFeature = [&](float offX, float offY, float& outX,
                                  float& outY) {
        float dx = offX;
        float dy = offY;
        rotate(dx, dy, roll);
        outX = cx + dx;
        outY = cy + dy;
    };

    // Eyes (sclera + pupil). Height collapses toward a thin line as the eye
    // closes, so a blink is obvious.
    const auto drawEye = [&](float sideSign, float openness, float browUp) {
        float ex = 0.0F;
        float ey = 0.0F;
        placeFeature(sideSign * headR * 0.40F, -headR * 0.18F, ex, ey);
        const float eyeRx = headR * 0.22F;
        const float eyeRy = std::max(headR * 0.03F, headR * 0.18F * openness);
        canvas.fillEllipse(ex, ey, eyeRx, eyeRy, Bgra{250, 250, 250, 255});
        if (openness > 0.2F) {
            const float pupilR = headR * 0.09F;
            canvas.fillEllipse(ex, ey, pupilR, std::min(pupilR, eyeRy),
                               Bgra{60, 45, 35, 255});
        }
        // Eyebrow rises with browUp.
        float bx = 0.0F;
        float by = 0.0F;
        placeFeature(sideSign * headR * 0.40F,
                     -headR * (0.44F + browUp * 0.14F), bx, by);
        canvas.fillEllipse(bx, by, headR * 0.24F, headR * 0.05F,
                           Bgra{70, 60, 55, 255});
    };
    drawEye(-1.0F, eyeOpenLeft, browUpLeft);
    drawEye(1.0F, eyeOpenRight, browUpRight);

    // Nose.
    float nx = 0.0F;
    float ny = 0.0F;
    placeFeature(0.0F, headR * 0.05F, nx, ny);
    canvas.fillEllipse(nx, ny, headR * 0.07F, headR * 0.12F, Bgra{120, 160, 210, 255});

    // Mouth: widens with mouthWide, opens (taller + darker interior) with
    // mouthOpen.
    float mx = 0.0F;
    float my = 0.0F;
    placeFeature(0.0F, headR * 0.48F, mx, my);
    const float mouthRx = headR * (0.20F + 0.16F * mouthWide);
    const float mouthRy = headR * (0.04F + 0.30F * mouthOpen);
    canvas.fillEllipse(mx, my, mouthRx, mouthRy, Bgra{70, 70, 190, 255});
    if (mouthOpen > 0.15F) {
        canvas.fillEllipse(mx, my, mouthRx * 0.7F, mouthRy * 0.7F,
                           Bgra{50, 40, 110, 255});
    }

    const auto stride = width_ * 4U;
    return AvatarRenderFrame::fromBgra(timestamp, width_, height_, stride,
                                       canvas.take());
}

}  // namespace creator::avatar
