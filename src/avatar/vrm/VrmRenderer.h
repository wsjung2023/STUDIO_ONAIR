#pragma once

#include "avatar/AvatarRenderFrame.h"
#include "avatar/vrm/GltfDocument.h"
#include "avatar/vrm/GltfMath.h"
#include "core/Result.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace creator::avatar::vrm {

/// A decoded RGBA texture (straight alpha), supplied by the app layer (which has
/// an image codec) so this Qt-free renderer never links one itself.
struct DecodedTexture final {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::vector<std::uint8_t> rgba;  // width*height*4, straight alpha
};

/// Per-frame inputs to the VRM software renderer.
struct VrmRenderParams final {
    // Head orientation from tracking, in radians (applied to the humanoid head
    // bone and its descendants when one is identified).
    float headYaw{0};
    float headPitch{0};
    float headRoll{0};
    // Named VRM expression weights in [0,1] (e.g. "blink", "aa"); unknown names
    // are ignored. Empty = neutral.
    std::vector<std::pair<std::string, float>> expressions;
};

/// A minimal CPU renderer for a loaded glTF/VRM model: it flattens the node
/// hierarchy, skins and morphs the meshes, projects them through a perspective
/// camera auto-framed on the head-and-upper-body, and depth-rasterises textured,
/// cel-shaded triangles to a straight-alpha BGRA AvatarRenderFrame. It is
/// deliberately Qt- and GPU-free so it drops into the existing avatar pipeline.
class VrmRenderer final {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<VrmRenderer>> open(
        GltfDocument document, std::vector<DecodedTexture> textures,
        std::uint32_t width, std::uint32_t height);

    [[nodiscard]] core::Result<AvatarRenderFrame> render(
        core::TimestampNs timestamp, const VrmRenderParams& params);

private:
    VrmRenderer(GltfDocument document, std::vector<DecodedTexture> textures,
                std::uint32_t width, std::uint32_t height);

    GltfDocument doc_;
    std::vector<DecodedTexture> textures_;
    std::uint32_t width_;
    std::uint32_t height_;
    // Parent index per node (-1 = root) and the head-bone node index (-1 = none),
    // resolved once at open() from the hierarchy / VRM humanoid extension.
    std::vector<int> parent_;
    int headNode_{-1};
    // Framing computed from the rest-pose bounds so the head fills the frame.
    Vec3 boundsMin_{}, boundsMax_{};
};

}  // namespace creator::avatar::vrm
