#pragma once

#include "avatar/AvatarParameterMapper.h"
#include "avatar/AvatarRenderFrame.h"
#include "avatar/IAvatarRenderer.h"
#include "core/Result.h"
#include "core/Timebase.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace creator::avatar {

/// A rectangle in portrait-normalised coordinates (fractions of the square).
struct PortraitRegion final {
    float x{0.0F};
    float y{0.0F};
    float width{0.0F};
    float height{0.0F};
};

/// A 2.5D portrait: three aligned full images of the same character (eyes
/// open + mouth closed, eyes closed, mouth open) plus the eye/mouth rectangles
/// that are swapped in. Straight-alpha BGRA, decoded by the app (Qt owns the
/// image codecs); this module stays Qt-free.
struct PortraitLayers final {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::vector<std::uint8_t> neutralBgra;
    std::vector<std::uint8_t> blinkBgra;
    std::vector<std::uint8_t> speakBgra;
    PortraitRegion eyes;   // both eyes; left = first half, right = second half
    PortraitRegion mouth;
};

/// Renders a 2.5D portrait avatar from the nine canonical tracking channels:
/// closed eyes and an open mouth are image-region swaps (never blended), the
/// head translates/rotates slightly with yaw/pitch/roll, and the whole bust
/// "breathes" on the project timebase so it never looks like a dead picture.
/// The same approach as a video-meeting portrait avatar: cheap (CPU, no GPU),
/// readable at thumbnail size, and honest about being a drawn character.
class PortraitAvatarRenderer final : public IAvatarRenderer {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<PortraitAvatarRenderer>> create(
        PortraitLayers layers, std::uint32_t width, std::uint32_t height);

    [[nodiscard]] core::Result<AvatarRenderFrame> render(
        core::TimestampNs timestamp,
        std::span<const AvatarParameterValue> parameters) override;

    /// The portrait square's side as a fraction of the frame height.
    static constexpr float kPortraitHeightFraction = 0.92F;
    /// eyeOpen below this shows the closed-eye layer (blink >= 0.6 upstream).
    static constexpr float kEyeClosedThreshold = 0.4F;
    /// mouthOpen at or above this shows the open-mouth layer.
    static constexpr float kMouthOpenThreshold = 0.45F;

private:
    PortraitAvatarRenderer(PortraitLayers layers, std::uint32_t width, std::uint32_t height);

    PortraitLayers layers_;
    std::uint32_t width_;
    std::uint32_t height_;
    std::vector<std::uint8_t> composed_;
};

}  // namespace creator::avatar
