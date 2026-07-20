#pragma once

#include "avatar/AvatarParameterMapper.h"
#include "avatar/IAvatarRenderer.h"

#include <cstdint>
#include <vector>

namespace creator::avatar {

/// Canonical model-parameter names the placeholder avatar understands. They are
/// the identity image of the nine ExpressionParameters channels, so the
/// AvatarParameterMapper produced by placeholderAvatarBindings() feeds this
/// renderer directly.
struct PlaceholderAvatarParameterNames final {
    static constexpr const char* kEyeOpenLeft = "placeholder.eyeOpenLeft";
    static constexpr const char* kEyeOpenRight = "placeholder.eyeOpenRight";
    static constexpr const char* kBrowUpLeft = "placeholder.browUpLeft";
    static constexpr const char* kBrowUpRight = "placeholder.browUpRight";
    static constexpr const char* kMouthOpen = "placeholder.mouthOpen";
    static constexpr const char* kMouthWide = "placeholder.mouthWide";
    static constexpr const char* kHeadYaw = "placeholder.headYaw";
    static constexpr const char* kHeadPitch = "placeholder.headPitch";
    static constexpr const char* kHeadRoll = "placeholder.headRoll";
};

/// The identity binding set that maps the nine canonical tracking channels onto
/// the placeholder model parameter names above.
[[nodiscard]] std::vector<AvatarParameterBinding> placeholderAvatarBindings();

/// A first-party, clearly-non-photoreal avatar renderer used when no real
/// Inochi2D runtime/model is available. It rasterizes a simple cartoon head
/// whose eyes blink, mouth opens, brows raise, and head turns/tilts directly
/// from the mapped expression parameters, so the full tracking -> render ->
/// composite -> record chain is real and visible without a bundled puppet
/// asset. It draws no photoreal face and must always be surfaced as a
/// placeholder in the UI (CLAUDE.md 9).
class PlaceholderAvatarRenderer final : public IAvatarRenderer {
public:
    PlaceholderAvatarRenderer(std::uint32_t width, std::uint32_t height)
        : width_(width), height_(height) {}

    [[nodiscard]] core::Result<AvatarRenderFrame> render(
        core::TimestampNs timestamp,
        std::span<const AvatarParameterValue> parameters) override;

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

private:
    std::uint32_t width_;
    std::uint32_t height_;
};

}  // namespace creator::avatar
