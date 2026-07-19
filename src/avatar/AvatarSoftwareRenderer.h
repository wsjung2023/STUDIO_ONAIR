#pragma once

#include "avatar/AvatarSoftwareRasterizer.h"
#include "avatar/IAvatarRenderer.h"

#include <functional>
#include <vector>

namespace creator::avatar {

struct AvatarSoftwareRenderInput final {
    std::vector<AvatarMeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    AvatarTexture texture;
};

/// IAvatarRenderer implementation backed by the validated CPU rasterizer.
/// Model adapters provide one frame's already-decoded mesh and texture data;
/// this class owns the timestamp/parameter handoff and frame construction.
class AvatarSoftwareRenderer final : public IAvatarRenderer {
public:
    using MeshProvider = std::function<core::Result<AvatarSoftwareRenderInput>(
        core::TimestampNs, std::span<const AvatarParameterValue>)>;

    AvatarSoftwareRenderer(std::uint32_t width, std::uint32_t height,
                           MeshProvider provider)
        : width_(width), height_(height), provider_(std::move(provider)) {}

    [[nodiscard]] core::Result<AvatarRenderFrame> render(
        core::TimestampNs timestamp,
        std::span<const AvatarParameterValue> parameters) override;

private:
    std::uint32_t width_;
    std::uint32_t height_;
    MeshProvider provider_;
};

}  // namespace creator::avatar
