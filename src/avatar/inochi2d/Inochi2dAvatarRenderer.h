#pragma once

#include "avatar/IAvatarRenderer.h"
#include "avatar/inochi2d/Inochi2dModelRuntime.h"

#include <filesystem>
#include <memory>
#include <optional>

namespace creator::avatar::inochi2d {

/// IAvatarRenderer adapter for the optional Inochi2D runtime and the shared
/// software draw-batch compositor.
class Inochi2dAvatarRenderer final : public IAvatarRenderer {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<Inochi2dAvatarRenderer>> open(
        const std::filesystem::path& libraryPath,
        const std::filesystem::path& modelPath, std::uint32_t width,
        std::uint32_t height);

    Inochi2dAvatarRenderer(std::unique_ptr<Inochi2dModelRuntime> runtime,
                           std::uint32_t width, std::uint32_t height)
        : runtime_(std::move(runtime)), width_(width), height_(height) {}

    [[nodiscard]] core::Result<AvatarRenderFrame> render(
        core::TimestampNs timestamp,
        std::span<const AvatarParameterValue> parameters) override;

private:
    std::unique_ptr<Inochi2dModelRuntime> runtime_;
    std::uint32_t width_;
    std::uint32_t height_;
    std::optional<core::TimestampNs> lastTimestamp_;
};

}  // namespace creator::avatar::inochi2d
