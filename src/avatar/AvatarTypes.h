#pragma once

#include "avatar/AvatarIdentifiers.h"

#include <compare>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace creator::avatar {

/// Supported runtime formats for an avatar specification.
enum class AvatarRepresentation { Inochi2d, Vrm1, GltfRig };

/// Skeleton families that determine which avatar representations are valid.
enum class RigFamily {
    Humanoid, Kemonomimi, AnthroBiped, Mascot, Quadruped, Avian
};

/// Named attachment points for an avatar's replaceable visual assets.
enum class AvatarSlot {
    Body, Head, Face, Skin, HairFront, HairSideLeft, HairSideRight,
    HairBack, HairTie, Brows, Eyes, Nose, Mouth, Teeth, EarLeft,
    EarRight, Muzzle, HornLeft, HornRight, WingLeft, WingRight, Tail,
    Inner, Top, Bottom, Outerwear, Hands, Footwear, Headwear,
    FaceAccessory, BodyAccessory
};

/// A versioned avatar asset selected for one avatar slot.
struct AssetRef final {
    AvatarAssetId assetId;
    std::string version;
    std::string variantId;

    friend bool operator==(const AssetRef&, const AssetRef&) = default;
};

/// A named normalized scalar value such as a morph target or expression.
struct NamedScalar final {
    std::string name;
    float value{};

    friend bool operator==(const NamedScalar&, const NamedScalar&) = default;
};

/// An RGBA color whose channels are normalized to the inclusive range [0, 1].
struct ColorRgba final {
    float red{};
    float green{};
    float blue{};
    float alpha{1.0F};

    friend bool operator==(const ColorRgba&, const ColorRgba&) = default;
};

/// Per-material normalized rendering-channel overrides for an avatar asset.
struct MaterialOverride final {
    std::string channel;
    ColorRgba baseColor;
    float metallic{};
    float roughness{1.0F};
    float emission{};
    float opacity{1.0F};

    friend bool operator==(const MaterialOverride&, const MaterialOverride&) = default;
};

}  // namespace creator::avatar
