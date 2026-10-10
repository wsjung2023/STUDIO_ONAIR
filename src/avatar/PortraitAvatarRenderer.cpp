#include "avatar/PortraitAvatarRenderer.h"

#include "avatar/PlaceholderAvatarRenderer.h"
#include "core/AppError.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string_view>

namespace creator::avatar {
namespace {

constexpr float kPi = 3.14159265358979323846F;

float valueOf(std::span<const AvatarParameterValue> parameters, std::string_view name,
              float fallback) {
    for (const auto& p : parameters) {
        if (p.modelParameter == name) return p.value;
    }
    return fallback;
}

float clampUnit(float v) noexcept { return std::clamp(v, 0.0F, 1.0F); }
float clampBipolar(float v) noexcept { return std::clamp(v, -1.0F, 1.0F); }

// Copies `region` (normalised) of `from` over `into`; both are W×H BGRA.
void swapRegion(std::vector<std::uint8_t>& into, const std::vector<std::uint8_t>& from,
                std::uint32_t width, std::uint32_t height, const PortraitRegion& region) {
    const auto x0 = static_cast<std::uint32_t>(std::clamp(region.x, 0.0F, 1.0F) * width);
    const auto y0 = static_cast<std::uint32_t>(std::clamp(region.y, 0.0F, 1.0F) * height);
    const auto x1 = static_cast<std::uint32_t>(
        std::clamp(region.x + region.width, 0.0F, 1.0F) * width);
    const auto y1 = static_cast<std::uint32_t>(
        std::clamp(region.y + region.height, 0.0F, 1.0F) * height);
    for (std::uint32_t y = y0; y < y1; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * width * 4U;
        std::copy(from.begin() + static_cast<std::ptrdiff_t>(row + x0 * 4U),
                  from.begin() + static_cast<std::ptrdiff_t>(row + x1 * 4U),
                  into.begin() + static_cast<std::ptrdiff_t>(row + x0 * 4U));
    }
}

}  // namespace

core::Result<std::unique_ptr<PortraitAvatarRenderer>> PortraitAvatarRenderer::create(
    PortraitLayers layers, std::uint32_t width, std::uint32_t height) {
    if (width == 0U || height == 0U) {
        return core::AppError{core::ErrorCode::InvalidArgument, "Portrait frame is empty"};
    }
    if (layers.width == 0U || layers.height == 0U) {
        return core::AppError{core::ErrorCode::InvalidArgument, "Portrait image is empty"};
    }
    const std::size_t expected = static_cast<std::size_t>(layers.width) * layers.height * 4U;
    if (layers.neutralBgra.size() != expected || layers.blinkBgra.size() != expected ||
        layers.speakBgra.size() != expected) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Portrait layers must be three aligned BGRA images of one size"};
    }
    return std::unique_ptr<PortraitAvatarRenderer>{
        new PortraitAvatarRenderer{std::move(layers), width, height}};
}

PortraitAvatarRenderer::PortraitAvatarRenderer(PortraitLayers layers, std::uint32_t width,
                                               std::uint32_t height)
    : layers_(std::move(layers)), width_(width), height_(height) {}

core::Result<AvatarRenderFrame> PortraitAvatarRenderer::render(
    core::TimestampNs timestamp, std::span<const AvatarParameterValue> parameters) {
    using Names = PlaceholderAvatarParameterNames;
    const float eyeL = clampUnit(valueOf(parameters, Names::kEyeOpenLeft, 1.0F));
    const float eyeR = clampUnit(valueOf(parameters, Names::kEyeOpenRight, 1.0F));
    const float mouth = clampUnit(valueOf(parameters, Names::kMouthOpen, 0.0F));
    const float yaw = clampBipolar(valueOf(parameters, Names::kHeadYaw, 0.0F));
    const float pitch = clampBipolar(valueOf(parameters, Names::kHeadPitch, 0.0F));
    const float roll = clampBipolar(valueOf(parameters, Names::kHeadRoll, 0.0F));

    // ---- 1. Expression: binary region swaps on a copy of the neutral image ----
    composed_ = layers_.neutralBgra;
    const PortraitRegion leftEye{layers_.eyes.x, layers_.eyes.y, layers_.eyes.width / 2.0F,
                                 layers_.eyes.height};
    const PortraitRegion rightEye{layers_.eyes.x + layers_.eyes.width / 2.0F, layers_.eyes.y,
                                  layers_.eyes.width / 2.0F, layers_.eyes.height};
    if (eyeL < kEyeClosedThreshold) {
        swapRegion(composed_, layers_.blinkBgra, layers_.width, layers_.height, leftEye);
    }
    if (eyeR < kEyeClosedThreshold) {
        swapRegion(composed_, layers_.blinkBgra, layers_.width, layers_.height, rightEye);
    }
    if (mouth >= kMouthOpenThreshold) {
        swapRegion(composed_, layers_.speakBgra, layers_.width, layers_.height, layers_.mouth);
    }

    // ---- 2. Head motion + breathing (small, like a meeting-tile portrait) ----
    const float t = std::chrono::duration<float>(timestamp.time_since_epoch()).count();
    const float side = kPortraitHeightFraction * static_cast<float>(height_);
    // Breathing: 3.4 s ease-in-out loop, +1px..-3px at 256px, 1.0..1.012 scale.
    const float phase = 0.5F - 0.5F * std::cos(2.0F * kPi * std::fmod(t, 3.4F) / 3.4F);
    const float breatheDy = (1.0F - 4.0F * phase) * (side / 256.0F);
    const float breatheScale = 1.0F + 0.012F * phase;
    // Yaw/pitch become a small shift (we track no translation) + foreshortening.
    const float yawDeg = std::clamp(yaw * 18.0F * 0.16F, -3.0F, 3.0F);
    const float pitchDeg = std::clamp(pitch * 18.0F * 0.12F, -2.2F, 2.2F);
    const float rollDeg = std::clamp(roll * 18.0F * 0.12F, -2.2F, 2.2F);
    const float dx = -yaw * 0.03F * side;
    const float dy = pitch * 0.02F * side + breatheDy;
    const float scaleX = breatheScale * std::cos(yawDeg * kPi / 180.0F);
    const float scaleY = breatheScale * std::cos(pitchDeg * kPi / 180.0F);
    const float cosR = std::cos(rollDeg * kPi / 180.0F);
    const float sinR = std::sin(rollDeg * kPi / 180.0F);

    // Destination: the portrait square centred at (cx, cy), scaled and rotated
    // about its (0.5, 0.62) point like the reference CSS transform-origin.
    const float cx = static_cast<float>(width_) / 2.0F + dx;
    const float cy = static_cast<float>(height_) / 2.0F + dy;
    const float pxPerSrc = side / static_cast<float>(layers_.width);
    const float pyPerSrc = side / static_cast<float>(layers_.height);
    // Transform origin in portrait-local pixels and in frame pixels.
    const float originLocalX = 0.5F * side;
    const float originLocalY = 0.62F * side;
    const float originFrameX = cx - 0.5F * side + originLocalX;
    const float originFrameY = cy - 0.5F * side + originLocalY;

    std::vector<std::uint8_t> out(static_cast<std::size_t>(width_) * height_ * 4U, 0);
    const int bound = static_cast<int>(std::ceil(side * 0.75F)) + 4;
    const int xStart = std::max(0, static_cast<int>(cx) - bound);
    const int xEnd = std::min(static_cast<int>(width_), static_cast<int>(cx) + bound);
    const int yStart = std::max(0, static_cast<int>(cy) - bound);
    const int yEnd = std::min(static_cast<int>(height_), static_cast<int>(cy) + bound);
    const auto W = static_cast<int>(layers_.width);
    const auto H = static_cast<int>(layers_.height);
    for (int y = yStart; y < yEnd; ++y) {
        for (int x = xStart; x < xEnd; ++x) {
            // Inverse transform: frame pixel -> rotate back -> unscale -> portrait-local.
            const float fx = (static_cast<float>(x) + 0.5F) - originFrameX;
            const float fy = (static_cast<float>(y) + 0.5F) - originFrameY;
            const float rx = cosR * fx + sinR * fy;
            const float ry = -sinR * fx + cosR * fy;
            const float lx = rx / scaleX + originLocalX;
            const float ly = ry / scaleY + originLocalY;
            const float u = lx / pxPerSrc - 0.5F;
            const float v = ly / pyPerSrc - 0.5F;
            const int u0 = static_cast<int>(std::floor(u));
            const int v0 = static_cast<int>(std::floor(v));
            if (u0 < -1 || v0 < -1 || u0 >= W || v0 >= H) continue;
            const float fu = u - static_cast<float>(u0);
            const float fv = v - static_cast<float>(v0);
            float acc[4] = {0, 0, 0, 0};
            float wsum = 0.0F;
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    const int su = u0 + i;
                    const int sv = v0 + j;
                    if (su < 0 || sv < 0 || su >= W || sv >= H) continue;
                    const float w = (i ? fu : 1.0F - fu) * (j ? fv : 1.0F - fv);
                    const std::size_t at =
                        (static_cast<std::size_t>(sv) * layers_.width + static_cast<std::size_t>(su)) * 4U;
                    const float a = static_cast<float>(composed_[at + 3]) / 255.0F;
                    // Weight colour by alpha so transparent neighbours do not darken edges.
                    acc[0] += composed_[at] * w * a;
                    acc[1] += composed_[at + 1] * w * a;
                    acc[2] += composed_[at + 2] * w * a;
                    acc[3] += 255.0F * w * a;
                    wsum += w;
                }
            }
            if (wsum <= 0.0F || acc[3] <= 0.0F) continue;
            const float alpha = acc[3] / wsum;  // 0..255
            const float inv = 255.0F / acc[3];  // un-premultiply colour
            const std::size_t o = (static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)) * 4U;
            out[o] = static_cast<std::uint8_t>(std::clamp(acc[0] * inv, 0.0F, 255.0F));
            out[o + 1] = static_cast<std::uint8_t>(std::clamp(acc[1] * inv, 0.0F, 255.0F));
            out[o + 2] = static_cast<std::uint8_t>(std::clamp(acc[2] * inv, 0.0F, 255.0F));
            out[o + 3] = static_cast<std::uint8_t>(std::clamp(alpha, 0.0F, 255.0F));
        }
    }
    return AvatarRenderFrame::fromBgra(timestamp, width_, height_, width_ * 4U, std::move(out));
}

}  // namespace creator::avatar
