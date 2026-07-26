#pragma once

#include "avatar/IAvatarRenderer.h"
#include "avatar/vrm/GltfDocument.h"
#include "avatar/vrm/VrmRenderer.h"

#include <memory>
#include <vector>

namespace creator::avatar::vrm {

/// IAvatarRenderer adapter over the VRM software renderer. It maps the canonical
/// tracking channels (head yaw/pitch/roll, eye open, mouth) onto the VRM head
/// bone and expression morphs, so a VRoid puppet is driven by the same tracking
/// chain as every other avatar. Textures arrive pre-decoded from the app layer.
class VrmAvatarRenderer final : public IAvatarRenderer {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<VrmAvatarRenderer>> open(
        GltfDocument document, std::vector<DecodedTexture> textures,
        std::uint32_t width, std::uint32_t height);

    [[nodiscard]] core::Result<AvatarRenderFrame> render(
        core::TimestampNs timestamp,
        std::span<const AvatarParameterValue> parameters) override;

private:
    explicit VrmAvatarRenderer(std::unique_ptr<VrmRenderer> renderer)
        : renderer_(std::move(renderer)) {}

    std::unique_ptr<VrmRenderer> renderer_;
};

}  // namespace creator::avatar::vrm
