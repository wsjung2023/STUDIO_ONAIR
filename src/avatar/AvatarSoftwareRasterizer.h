#pragma once

#include "avatar/AvatarRenderFrame.h"

#include <cstdint>
#include <span>
#include <vector>

namespace creator::avatar {

struct AvatarMeshVertex final {
    float x{0.0F};
    float y{0.0F};
    float u{0.0F};
    float v{0.0F};
};

struct AvatarTexture final {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::vector<std::uint8_t> bgra;
};

// Inochi2D compositing modes this rasteriser understands. Values match the
// Inochi2D BlendMode enum ordinals emitted by the runtime's draw commands.
enum class AvatarBlendMode : std::uint32_t {
    Normal = 0,
    Multiply = 1,
    ClipToLower = 17,   // draw only within the alpha already below (mask-in)
    SliceFromLower = 18,  // cut the alpha already below (mask-out)
};

struct AvatarSoftwareRenderInput final {
    std::vector<AvatarMeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    AvatarTexture texture;
    // Raw Inochi2D blend-mode ordinal; unknown modes fall back to Normal.
    std::uint32_t blendMode{0};
};

/// Small CPU fallback for draw-list triangles. It is deliberately independent
/// of Inochi2D headers: an adapter converts the SDK's vertices, indices, and
/// texture bytes into this stable representation before calling render().
class AvatarSoftwareRasterizer final {
public:
    [[nodiscard]] static core::Result<AvatarRenderFrame> render(
        core::TimestampNs timestamp, std::uint32_t width,
        std::uint32_t height, std::span<const AvatarMeshVertex> vertices,
        std::span<const std::uint32_t> indices, const AvatarTexture& texture);
    [[nodiscard]] static core::Result<AvatarRenderFrame> renderBatches(
        core::TimestampNs timestamp, std::uint32_t width,
        std::uint32_t height, std::span<const AvatarSoftwareRenderInput> batches);
};

}  // namespace creator::avatar
