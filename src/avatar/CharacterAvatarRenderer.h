#pragma once

#include "avatar/AvatarParameterMapper.h"
#include "avatar/IAvatarRenderer.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace creator::avatar {

/// Built-in, selectable VTuber characters. Each is a clean, layered 2D vector
/// rig drawn by CharacterAvatarRenderer and driven live by the nine
/// ExpressionParameters channels. Add a new character by extending this enum,
/// registering it in avatarCharacterCatalog(), and giving it a draw function.
enum class AvatarCharacterId : std::uint8_t {
    Human = 0,   // 사람
    Cat = 1,     // 고양이
    Fox = 2,     // 여우
    ManMid = 3,  // 아저씨 (middle-aged man)
    Woman = 4,   // 여자 (adult woman)
    Boy = 5,     // 남자애 (little boy)
    Girl = 6,    // 여자애 (little girl)
    Teen = 7,    // 소녀(중학생) (teen schoolgirl)
    Dog = 8,     // 강아지 (dog)
    Bear = 9,    // 곰 (bear)
    Alien = 10,  // 외계인 (alien)
};

/// Where the avatar sits in the rendered frame.
///  - Front  : the character is centred and large — the whole frame is avatar.
///  - Corner : the frame is transparent and the character is a ~1/4-size
///             picture-in-picture in one corner, so it composites over a screen
///             recording.
enum class AvatarPlacementMode : std::uint8_t { Front = 0, Corner = 1 };

/// Which corner the Corner placement uses.
enum class AvatarCorner : std::uint8_t {
    LeftBottom = 0,
    RightBottom = 1,
    LeftTop = 2,
    RightTop = 3,
};

struct AvatarPlacement final {
    AvatarPlacementMode mode{AvatarPlacementMode::Front};
    AvatarCorner corner{AvatarCorner::RightBottom};
};

/// Registry entry: a stable id, a machine key, and the user-facing Korean label.
struct AvatarCharacterInfo final {
    AvatarCharacterId id;
    std::string key;      // "human", "cat", "fox"
    std::string labelKo;  // "사람", "고양이", "여우"
};

/// The built-in character catalog, in display order.
[[nodiscard]] std::vector<AvatarCharacterInfo> avatarCharacterCatalog();

/// The identity binding set mapping the nine canonical tracking channels onto
/// the parameter names this renderer reads. Shared with the placeholder so the
/// same AvatarParameterMapper drives either renderer.
[[nodiscard]] std::vector<AvatarParameterBinding> characterAvatarBindings();

/// A first-party, appealing avatar renderer.
///
/// It rasterises one of several layered, cartoon VTuber characters (human, cat,
/// fox) on the CPU with anti-aliasing and alpha blending, so the full
/// tracking -> render -> composite -> record chain stays real and Qt-free while
/// looking intentional rather than like programmer art. Every rig layer is
/// driven directly by the mapped expression parameters: eyelids close on blink,
/// pupils drift with head yaw, the mouth opens/widens, eyebrows raise, and the
/// whole head tilts/turns with yaw/pitch/roll.
///
/// The character and placement can be swapped live (they are read atomically at
/// the top of each render), which is what the Studio avatar picker and the
/// front/corner placement controls drive.
class CharacterAvatarRenderer final : public IAvatarRenderer {
public:
    CharacterAvatarRenderer(std::uint32_t width, std::uint32_t height,
                            AvatarCharacterId character = AvatarCharacterId::Human,
                            AvatarPlacement placement = {},
                            std::uint32_t supersample = 2);

    [[nodiscard]] core::Result<AvatarRenderFrame> render(
        core::TimestampNs timestamp,
        std::span<const AvatarParameterValue> parameters) override;

    void setCharacter(AvatarCharacterId character) noexcept {
        character_.store(character, std::memory_order_relaxed);
    }
    [[nodiscard]] AvatarCharacterId character() const noexcept {
        return character_.load(std::memory_order_relaxed);
    }

    // Placement mode selects only the BACKDROP style now: Front paints an opaque
    // gradient (the whole frame is avatar); Corner leaves the frame transparent
    // so it composites over a screen recording. The actual position and size come
    // from the free transform below. Switching mode also snaps the transform to a
    // convenient preset (applyFrontPreset / applyCornerPreset), but any later
    // drag/resize overrides it.
    void setPlacementMode(AvatarPlacementMode mode) noexcept {
        placementMode_.store(mode, std::memory_order_relaxed);
    }
    void setPlacementCorner(AvatarCorner corner) noexcept {
        placementCorner_.store(corner, std::memory_order_relaxed);
    }
    [[nodiscard]] AvatarPlacement placement() const noexcept {
        return {placementMode_.load(std::memory_order_relaxed),
                placementCorner_.load(std::memory_order_relaxed)};
    }

    // ---- Free transform (thread-safe, read atomically each render) ----------
    //
    // userScale multiplies the base head radius (min(w,h)*0.30); posX/posY are the
    // normalised head centre in [0,1] of the frame. These are what a live drag and
    // the size slider drive, and they always win over the placement presets.

    static constexpr float kMinScale = 0.35F;
    static constexpr float kMaxScale = 2.5F;

    void setUserScale(float scale) noexcept {
        userScale_.store(clampScale(scale), std::memory_order_relaxed);
    }
    [[nodiscard]] float userScale() const noexcept {
        return userScale_.load(std::memory_order_relaxed);
    }
    void setPosition(float nx, float ny) noexcept {
        posX_.store(clamp01(nx), std::memory_order_relaxed);
        posY_.store(clamp01(ny), std::memory_order_relaxed);
    }
    [[nodiscard]] float posX() const noexcept {
        return posX_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float posY() const noexcept {
        return posY_.load(std::memory_order_relaxed);
    }

    // ---- Hair/fur colour customisation --------------------------------------
    //
    // Packed 0x01RRGGBB: bit 24 marks "override set" so pure black is distinct
    // from "no override" (0 = each character's own default hair/fur colour).

    void setHairColor(int red, int green, int blue) noexcept {
        const auto clamp8 = [](int v) {
            return static_cast<std::uint32_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        };
        hairColor_.store(0x01000000U | (clamp8(red) << 16) | (clamp8(green) << 8) |
                             clamp8(blue),
                         std::memory_order_relaxed);
    }
    void clearHairColor() noexcept {
        hairColor_.store(0U, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint32_t hairColor() const noexcept {
        return hairColor_.load(std::memory_order_relaxed);
    }

    /// Snap to the centred, large, opaque-backdrop "정면" preset.
    void applyFrontPreset() noexcept;
    /// Snap to one of the four smaller, transparent-overlay corner presets.
    void applyCornerPreset(AvatarCorner corner) noexcept;

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

private:
    static float clamp01(float v) noexcept {
        return v < 0.0F ? 0.0F : (v > 1.0F ? 1.0F : v);
    }
    static float clampScale(float v) noexcept {
        return v < kMinScale ? kMinScale : (v > kMaxScale ? kMaxScale : v);
    }

    std::uint32_t width_;
    std::uint32_t height_;
    std::uint32_t supersample_;
    std::atomic<AvatarCharacterId> character_;
    std::atomic<AvatarPlacementMode> placementMode_;
    std::atomic<AvatarCorner> placementCorner_;
    std::atomic<float> userScale_{1.0F};
    std::atomic<float> posX_{0.5F};
    std::atomic<float> posY_{0.46F};
    std::atomic<std::uint32_t> hairColor_{0};  // 0 = per-character default
};

}  // namespace creator::avatar
