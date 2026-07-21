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
    Human = 0,  // 사람
    Cat = 1,    // 고양이
    Fox = 2,    // 여우
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

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

private:
    std::uint32_t width_;
    std::uint32_t height_;
    std::uint32_t supersample_;
    std::atomic<AvatarCharacterId> character_;
    std::atomic<AvatarPlacementMode> placementMode_;
    std::atomic<AvatarCorner> placementCorner_;
};

}  // namespace creator::avatar
