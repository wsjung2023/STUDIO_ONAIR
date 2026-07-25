#pragma once

#include "avatar/IAvatarRenderer.h"
#include "avatar/inochi2d/Inochi2dModelRuntime.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace creator::avatar::inochi2d {

/// IAvatarRenderer adapter for the optional Inochi2D runtime and the shared
/// software draw-batch compositor.
class Inochi2dAvatarRenderer final : public IAvatarRenderer {
public:
    // Size multiplier range for the live "avatar bigger/smaller" control.
    static constexpr float kMinScale = 0.25F;
    static constexpr float kMaxScale = 3.0F;

    [[nodiscard]] static core::Result<std::unique_ptr<Inochi2dAvatarRenderer>> open(
        const std::filesystem::path& runtimeRoot,
        const std::filesystem::path& modelPath, std::uint32_t width,
        std::uint32_t height);

    Inochi2dAvatarRenderer(std::unique_ptr<Inochi2dModelRuntime> runtime,
                           std::uint32_t width, std::uint32_t height)
        : runtime_(std::move(runtime)), width_(width), height_(height) {}

    [[nodiscard]] core::Result<AvatarRenderFrame> render(
        core::TimestampNs timestamp,
        std::span<const AvatarParameterValue> parameters) override;

    // Live free transform. These are read by the render thread and written by the
    // UI thread, so they are atomic and require no external lock. scale multiplies
    // the fitted size; posX/posY are the normalised [0,1] frame position of the
    // avatar's centre. The editor drives these from a size slider and a drag.
    void setUserScale(float scale) noexcept {
        userScale_.store(std::clamp(scale, kMinScale, kMaxScale),
                         std::memory_order_relaxed);
    }
    void setPosition(float posX, float posY) noexcept {
        posX_.store(std::clamp(posX, 0.0F, 1.0F), std::memory_order_relaxed);
        posY_.store(std::clamp(posY, 0.0F, 1.0F), std::memory_order_relaxed);
    }
    [[nodiscard]] float userScale() const noexcept {
        return userScale_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float posX() const noexcept {
        return posX_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float posY() const noexcept {
        return posY_.load(std::memory_order_relaxed);
    }

private:
    std::unique_ptr<Inochi2dModelRuntime> runtime_;
    std::uint32_t width_;
    std::uint32_t height_;
    std::optional<core::TimestampNs> lastTimestamp_;
    std::atomic<float> userScale_{1.0F};
    std::atomic<float> posX_{0.5F};
    std::atomic<float> posY_{0.5F};
};

/// Tracking-to-parameter bindings for Inochi2D puppets that follow the standard
/// Inochi2D parameter names (e.g. the CC0 Arch-chan model). Maps the canonical
/// tracking channels onto the model's one-dimensional expression parameters;
/// head position (a 2D parameter) is left to the model's own idle physics.
[[nodiscard]] std::vector<AvatarParameterBinding> inochi2dAvatarBindings();

}  // namespace creator::avatar::inochi2d
