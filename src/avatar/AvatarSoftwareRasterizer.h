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

// Inochi2D blend-mode ordinals emitted by the runtime's draw commands
// (in_blend_mode_t). Unknown modes fall back to Normal.
enum class AvatarBlendMode : std::uint32_t {
    Normal = 0x00,
    Multiply = 0x01,
    Screen = 0x02,
    Overlay = 0x03,
    Darken = 0x04,
    Lighten = 0x05,
    ColorDodge = 0x06,
    LinearDodge = 0x07,  // add
    AddGlow = 0x08,
    ColorBurn = 0x09,
    HardLight = 0x0A,
    SoftLight = 0x0B,
    Difference = 0x0C,
    Exclusion = 0x0D,
    Subtract = 0x0E,
    Inverse = 0x0F,
    DestinationIn = 0x10,  // keep dst only where src is present
    SourceIn = 0x11,       // keep src only where dst is present (clip to lower)
    SourceOut = 0x12,      // keep src only where dst is absent
};

// Inochi2D draw-command state (in_drawstate_t, v0.8.7 CFFI). Drives the
// stateful compositor: masks are defined then consumed, and composite groups
// render children to an offscreen buffer that is blitted back with a blend mode.
enum class AvatarDrawState : std::uint32_t {
    Normal = 0,
    DefineMask = 1,
    MaskedDraw = 2,
    CompositeBegin = 3,
    CompositeEnd = 4,
    CompositeBlit = 5,
};

enum class AvatarMaskMode : std::uint32_t {
    Mask = 0,   // draw only where the mask is present
    Dodge = 1,  // draw only where the mask is absent
};

struct AvatarSoftwareRenderInput final {
    // Raw Inochi2D draw state; the compositor interprets it (default Normal).
    std::uint32_t state{0};
    // Raw Inochi2D blend-mode ordinal; unknown modes fall back to Normal.
    std::uint32_t blendMode{0};
    // Raw Inochi2D mask mode (0 = mask, 1 = dodge).
    std::uint32_t maskMode{0};
    std::vector<AvatarMeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    // Empty (width/height 0) for control states such as composite blit.
    AvatarTexture texture;
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
