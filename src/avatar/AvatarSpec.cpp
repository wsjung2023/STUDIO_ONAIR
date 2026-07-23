#include "avatar/AvatarSpec.h"

#include "core/AppError.h"

#include <cmath>
#include <cstddef>
#include <optional>
#include <regex>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace creator::avatar {
namespace {

[[nodiscard]] core::AppError invalid(std::string message, std::string issueCode,
                                      std::string messageKey) {
    return core::AppError{core::ErrorCode::InvalidArgument, std::move(message),
                          std::move(issueCode), std::move(messageKey)};
}

[[nodiscard]] bool isContinuationByte(unsigned char byte) noexcept {
    return (byte & 0xC0U) == 0x80U;
}

/// Returns the Unicode code-point count when text is valid UTF-8.
[[nodiscard]] std::optional<std::size_t> utf8CodePointCount(std::string_view text) noexcept {
    std::size_t count = 0;
    for (std::size_t index = 0; index < text.size(); ++count) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }

        std::size_t continuationCount = 0;
        std::uint32_t codePoint = 0;
        if ((first & 0xE0U) == 0xC0U) {
            continuationCount = 1;
            codePoint = first & 0x1FU;
        } else if ((first & 0xF0U) == 0xE0U) {
            continuationCount = 2;
            codePoint = first & 0x0FU;
        } else if ((first & 0xF8U) == 0xF0U) {
            continuationCount = 3;
            codePoint = first & 0x07U;
        } else {
            return std::nullopt;
        }

        if (index + continuationCount >= text.size()) {
            return std::nullopt;
        }
        for (std::size_t offset = 1; offset <= continuationCount; ++offset) {
            const auto byte = static_cast<unsigned char>(text[index + offset]);
            if (!isContinuationByte(byte)) {
                return std::nullopt;
            }
            codePoint = (codePoint << 6U) | (byte & 0x3FU);
        }

        const auto minimumCodePoint = continuationCount == 1 ? 0x80U
            : continuationCount == 2 ? 0x800U : 0x10000U;
        if (codePoint < minimumCodePoint || codePoint > 0x10FFFFU ||
            (codePoint >= 0xD800U && codePoint <= 0xDFFFU)) {
            return std::nullopt;
        }
        index += continuationCount + 1;
    }
    return count;
}

[[nodiscard]] bool isValidName(std::string_view value, std::size_t maximumLength = 200U) noexcept {
    const auto length = utf8CodePointCount(value);
    return length.has_value() && *length >= 1U && *length <= maximumLength;
}

[[nodiscard]] bool isFiniteInRange(float value, float minimum, float maximum) noexcept {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

[[nodiscard]] bool hasValidColor(const ColorRgba& color) noexcept {
    return isFiniteInRange(color.red, 0.0F, 1.0F) &&
           isFiniteInRange(color.green, 0.0F, 1.0F) &&
           isFiniteInRange(color.blue, 0.0F, 1.0F) &&
           isFiniteInRange(color.alpha, 0.0F, 1.0F);
}

[[nodiscard]] bool hasValidNamedScalars(const std::vector<NamedScalar>& values) {
    std::unordered_set<std::string> names;
    for (const auto& value : values) {
        if (!isValidName(value.name) || !isFiniteInRange(value.value, -1.0F, 1.0F) ||
            !names.insert(value.name).second) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool hasValidMaterials(const std::vector<MaterialOverride>& materials) {
    std::unordered_set<std::string> channels;
    for (const auto& material : materials) {
        if (!isValidName(material.channel) || !channels.insert(material.channel).second ||
            !hasValidColor(material.baseColor) ||
            !isFiniteInRange(material.metallic, 0.0F, 1.0F) ||
            !isFiniteInRange(material.roughness, 0.0F, 1.0F) ||
            !isFiniteInRange(material.emission, 0.0F, 1.0F) ||
            !isFiniteInRange(material.opacity, 0.0F, 1.0F)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool hasRequiredSlots(const std::map<AvatarSlot, AssetRef>& slots) noexcept {
    return slots.contains(AvatarSlot::Body) && slots.contains(AvatarSlot::Head) &&
           slots.contains(AvatarSlot::Eyes) && slots.contains(AvatarSlot::Mouth);
}

[[nodiscard]] bool hasCompatibleRepresentation(RigFamily rigFamily,
                                                 AvatarRepresentation representation) noexcept {
    if (rigFamily == RigFamily::Quadruped || rigFamily == RigFamily::Avian) {
        return representation == AvatarRepresentation::GltfRig;
    }
    if (representation == AvatarRepresentation::Vrm1) {
        return rigFamily == RigFamily::Humanoid || rigFamily == RigFamily::Kemonomimi ||
               rigFamily == RigFamily::AnthroBiped;
    }
    return true;
}

[[nodiscard]] bool hasValidAssetReferences(const std::map<AvatarSlot, AssetRef>& slots) {
    static const std::regex semanticVersion{"^[0-9]+\\.[0-9]+\\.[0-9]+$"};
    for (const auto& [slot, asset] : slots) {
        static_cast<void>(slot);
        if (!std::regex_match(asset.version, semanticVersion) ||
            (!asset.variantId.empty() && !isValidName(asset.variantId))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool hasValidPalette(const std::map<std::string, ColorRgba>& palette) noexcept {
    for (const auto& [name, color] : palette) {
        if (!isValidName(name) || !hasValidColor(color)) {
            return false;
        }
    }
    return true;
}

}  // namespace

AvatarSpec::AvatarSpec(AvatarSpecDraft values) : values_(std::move(values)) {}

core::Result<AvatarSpec> AvatarSpec::create(AvatarSpecDraft draft) {
    if (!isValidName(draft.displayName)) {
        return invalid("avatar display name must be valid UTF-8 with 1 to 200 code points",
                       "avatar.spec.display-name", "avatar.validation.display-name");
    }
    if (!isValidName(draft.speciesFamily, 200U) || !isValidName(draft.styleTheme, 200U) ||
        !isValidName(draft.trackingProfileId, 200U)) {
        return invalid("avatar family, theme, and tracking profile must be valid UTF-8 with 1 to 200 code points",
                       "avatar.spec.named-field", "avatar.validation.named-field");
    }
    if (!hasCompatibleRepresentation(draft.rigFamily, draft.preferredRepresentation)) {
        return invalid("avatar representation is incompatible with rig family",
                       "avatar.spec.representation", "avatar.validation.representation");
    }
    if (!hasValidNamedScalars(draft.bodyMorphs) || !hasValidNamedScalars(draft.faceMorphs) ||
        !hasValidNamedScalars(draft.animalMorphs) ||
        !hasValidNamedScalars(draft.expressions) || !hasValidNamedScalars(draft.physics)) {
        return invalid("avatar named scalars must have unique valid names and normalized values",
                       "avatar.spec.named-scalar", "avatar.validation.named-scalar");
    }
    if (!hasValidAssetReferences(draft.slots)) {
        return invalid("avatar asset references must have semantic versions and valid variants",
                       "avatar.spec.asset-reference", "avatar.validation.asset-reference");
    }
    if (!hasValidPalette(draft.palette)) {
        return invalid("avatar palette names and colors must be valid", "avatar.spec.palette",
                       "avatar.validation.palette");
    }
    if (!hasValidMaterials(draft.materials)) {
        return invalid("avatar material overrides must have unique valid channels and normalized values",
                       "avatar.spec.material", "avatar.validation.material");
    }
    if (!hasRequiredSlots(draft.slots)) {
        return invalid("avatar requires body, head, eyes, and mouth slots",
                       "avatar.spec.required-slots", "avatar.validation.required-slots");
    }
    return AvatarSpec{std::move(draft)};
}

std::int32_t AvatarSpec::schemaVersion() const noexcept {
    return kCurrentSchemaVersion;
}

const AvatarId& AvatarSpec::avatarId() const noexcept {
    return values_.avatarId;
}

const std::string& AvatarSpec::displayName() const noexcept {
    return values_.displayName;
}

RigFamily AvatarSpec::rigFamily() const noexcept {
    return values_.rigFamily;
}

AvatarRepresentation AvatarSpec::preferredRepresentation() const noexcept {
    return values_.preferredRepresentation;
}

const std::map<AvatarSlot, AssetRef>& AvatarSpec::slots() const noexcept {
    return values_.slots;
}

const AvatarSpecDraft& AvatarSpec::values() const noexcept {
    return values_;
}

}  // namespace creator::avatar
