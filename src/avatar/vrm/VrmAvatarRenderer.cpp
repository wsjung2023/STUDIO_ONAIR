#include "avatar/vrm/VrmAvatarRenderer.h"

#include "avatar/PlaceholderAvatarRenderer.h"  // canonical parameter names

#include <algorithm>

namespace creator::avatar::vrm {
namespace {

float valueOf(std::span<const AvatarParameterValue> params, std::string_view name,
              float fallback) {
    for (const auto& p : params)
        if (p.modelParameter == name) return p.value;
    return fallback;
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

core::Result<std::unique_ptr<VrmAvatarRenderer>> VrmAvatarRenderer::open(
    GltfDocument document, std::vector<DecodedTexture> textures,
    std::uint32_t width, std::uint32_t height) {
    auto renderer =
        VrmRenderer::open(std::move(document), std::move(textures), width, height);
    if (!renderer.hasValue()) return renderer.error();
    return std::unique_ptr<VrmAvatarRenderer>{
        new VrmAvatarRenderer{std::move(renderer).value()}};
}

core::Result<AvatarRenderFrame> VrmAvatarRenderer::render(
    core::TimestampNs timestamp,
    std::span<const AvatarParameterValue> parameters) {
    using Names = PlaceholderAvatarParameterNames;
    VrmRenderParams p;
    // Tracking yaw/pitch/roll are in [-1, 1]; map to a natural head range so the
    // avatar turns/nods/tilts with the creator without over-rotating.
    p.headYaw = clampf(valueOf(parameters, Names::kHeadYaw, 0.0F), -1.0F, 1.0F) * 0.6F;
    p.headPitch = clampf(valueOf(parameters, Names::kHeadPitch, 0.0F), -1.0F, 1.0F) * 0.5F;
    p.headRoll = clampf(valueOf(parameters, Names::kHeadRoll, 0.0F), -1.0F, 1.0F) * 0.5F;

    // Expression morphs: forward the standard VRM expression names driven by the
    // eye-open and mouth channels. Renderers that lack these presets ignore them.
    const float blink =
        1.0F - clampf(std::min(valueOf(parameters, Names::kEyeOpenLeft, 1.0F),
                               valueOf(parameters, Names::kEyeOpenRight, 1.0F)),
                      0.0F, 1.0F);
    const float mouth = clampf(valueOf(parameters, Names::kMouthOpen, 0.0F), 0.0F, 1.0F);
    if (blink > 0.01F) p.expressions.emplace_back("blink", blink);
    if (mouth > 0.01F) p.expressions.emplace_back("aa", mouth);

    return renderer_->render(timestamp, p);
}

}  // namespace creator::avatar::vrm
