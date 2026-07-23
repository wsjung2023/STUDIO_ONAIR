#pragma once

#include "avatar/AvatarTypes.h"
#include "core/Result.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace creator::avatar {

/// Mutable input collected before AvatarSpec validates and owns an avatar's shared values.
struct AvatarSpecDraft final {
    AvatarId avatarId;
    std::string displayName;
    RigFamily rigFamily{RigFamily::Humanoid};
    std::string speciesFamily;
    std::string styleTheme;
    AvatarRepresentation preferredRepresentation{AvatarRepresentation::Inochi2d};
    std::vector<NamedScalar> bodyMorphs;
    std::vector<NamedScalar> faceMorphs;
    std::vector<NamedScalar> animalMorphs;
    std::map<AvatarSlot, AssetRef> slots;
    std::map<std::string, ColorRgba> palette;
    std::vector<MaterialOverride> materials;
    std::vector<NamedScalar> expressions;
    std::vector<NamedScalar> physics;
    std::string trackingProfileId;
};

/// Validated, engine-neutral avatar definition shared by all avatar renderers.
///
/// Instances can only be created through create(), which rejects malformed
/// text, invalid normalized values, incompatible rigs, and incomplete slots.
class AvatarSpec final {
public:
    static constexpr std::int32_t kCurrentSchemaVersion = 1;

    [[nodiscard]] static core::Result<AvatarSpec> create(AvatarSpecDraft draft);

    [[nodiscard]] std::int32_t schemaVersion() const noexcept;
    [[nodiscard]] const AvatarId& avatarId() const noexcept;
    [[nodiscard]] const std::string& displayName() const noexcept;
    [[nodiscard]] RigFamily rigFamily() const noexcept;
    [[nodiscard]] AvatarRepresentation preferredRepresentation() const noexcept;
    [[nodiscard]] const std::map<AvatarSlot, AssetRef>& slots() const noexcept;
    [[nodiscard]] const AvatarSpecDraft& values() const noexcept;

private:
    explicit AvatarSpec(AvatarSpecDraft values);

    AvatarSpecDraft values_;
};

}  // namespace creator::avatar
