#include "avatar/AvatarAssetManifest.h"

#include "core/AppError.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <regex>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace creator::avatar {
namespace {

core::AppError invalid(std::string message, std::string issueCode,
                       std::string messageKey) {
    return {core::ErrorCode::InvalidArgument, std::move(message),
            std::move(issueCode), std::move(messageKey)};
}

bool continuation(unsigned char byte) noexcept {
    return (byte & 0xC0U) == 0x80U;
}

std::optional<std::size_t> utf8Length(std::string_view text) noexcept {
    std::size_t count = 0;
    for (std::size_t index = 0; index < text.size(); ++count) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }
        std::size_t trailing = 0;
        std::uint32_t codePoint = 0;
        if ((first & 0xE0U) == 0xC0U) {
            trailing = 1;
            codePoint = first & 0x1FU;
        } else if ((first & 0xF0U) == 0xE0U) {
            trailing = 2;
            codePoint = first & 0x0FU;
        } else if ((first & 0xF8U) == 0xF0U) {
            trailing = 3;
            codePoint = first & 0x07U;
        } else {
            return std::nullopt;
        }
        if (index + trailing >= text.size()) return std::nullopt;
        for (std::size_t offset = 1; offset <= trailing; ++offset) {
            const auto byte = static_cast<unsigned char>(text[index + offset]);
            if (!continuation(byte)) return std::nullopt;
            codePoint = (codePoint << 6U) | (byte & 0x3FU);
        }
        const auto minimum = trailing == 1 ? 0x80U : trailing == 2 ? 0x800U : 0x10000U;
        if (codePoint < minimum || codePoint > 0x10FFFFU ||
            (codePoint >= 0xD800U && codePoint <= 0xDFFFU)) {
            return std::nullopt;
        }
        index += trailing + 1;
    }
    return count;
}

bool validText(std::string_view value, std::size_t maximum = 200U) noexcept {
    const auto length = utf8Length(value);
    return length.has_value() && *length > 0 && *length <= maximum;
}

bool validVersion(std::string_view value) {
    static const std::regex semanticVersion{"^[0-9]+\\.[0-9]+\\.[0-9]+$"};
    return std::regex_match(value.begin(), value.end(), semanticVersion);
}

bool validHash(std::string_view value) noexcept {
    return value.size() == 64U &&
           std::all_of(value.begin(), value.end(), [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

bool validPayloadPath(std::string_view value) noexcept {
    if (!validText(value, 1024U) || value.front() == '/' ||
        value.find('\0') != std::string_view::npos ||
        value.find('\\') != std::string_view::npos ||
        (value.size() >= 2U &&
         ((value[0] >= 'A' && value[0] <= 'Z') ||
          (value[0] >= 'a' && value[0] <= 'z')) &&
         value[1] == ':')) {
        return false;
    }
    std::size_t start = 0;
    while (start < value.size()) {
        const auto slash = value.find('/', start);
        const auto component =
            value.substr(start, slash == std::string_view::npos
                                    ? std::string_view::npos
                                    : slash - start);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (slash == std::string_view::npos) return true;
        start = slash + 1U;
    }
    return false;
}

template <typename Enum>
bool enumInRange(Enum value, Enum first, Enum last) noexcept {
    using Underlying = std::underlying_type_t<Enum>;
    const auto raw = static_cast<Underlying>(value);
    return raw >= static_cast<Underlying>(first) &&
           raw <= static_cast<Underlying>(last);
}

template <typename T>
bool hasDuplicates(const std::vector<T>& values) {
    auto sorted = values;
    std::sort(sorted.begin(), sorted.end());
    return std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end();
}

}  // namespace

AvatarAssetManifest::AvatarAssetManifest(AvatarAssetManifestDraft values)
    : values_(std::move(values)) {}

core::Result<AvatarAssetManifest> AvatarAssetManifest::create(
    AvatarAssetManifestDraft draft) {
    // Payload records intentionally carry only path and digest. Performance
    // byte counts are declared metadata; pack validation verifies them later
    // rather than creating a second size source of truth here.
    if (!validText(draft.packageId.value()) || !validText(draft.assetId.value()) ||
        !validText(draft.displayName) || !validText(draft.vendor) ||
        !validText(draft.sourceUri, 2048U) || !validText(draft.licenseId)) {
        return invalid("avatar asset identity and names must be non-empty valid UTF-8",
                       "avatar.asset.name", "avatar.validation.name");
    }
    if (!validVersion(draft.packageVersion) || !validVersion(draft.assetVersion) ||
        !validVersion(draft.licenseVersion) ||
        std::any_of(draft.dependencies.begin(), draft.dependencies.end(),
                    [](const AvatarAssetDependency& dependency) {
                        return !validText(dependency.assetId.value()) ||
                               !validVersion(dependency.version);
                    })) {
        return invalid("avatar asset versions must use major.minor.patch",
                       "avatar.asset.version", "avatar.validation.version");
    }
    if (draft.supportedRepresentations.empty() || draft.supportedRigFamilies.empty() ||
        draft.allowedSlots.empty() || draft.payloads.empty()) {
        return invalid("avatar asset requires a representation, rig, slot, and payload",
                       "avatar.asset.required-content", "avatar.validation.required-content");
    }
    if (std::any_of(draft.supportedRepresentations.begin(),
                    draft.supportedRepresentations.end(),
                    [](AvatarRepresentation value) {
                        return !enumInRange(value,
                                            AvatarRepresentation::Inochi2d,
                                            AvatarRepresentation::GltfRig);
                    }) ||
        std::any_of(draft.supportedRigFamilies.begin(),
                    draft.supportedRigFamilies.end(), [](RigFamily value) {
                        return !enumInRange(value, RigFamily::Humanoid,
                                            RigFamily::Avian);
                    }) ||
        std::any_of(draft.allowedSlots.begin(), draft.allowedSlots.end(),
                    [](AvatarSlot value) {
                        return !enumInRange(value, AvatarSlot::Body,
                                            AvatarSlot::BodyAccessory);
                    })) {
        return invalid("avatar asset compatibility contains an unknown enum value",
                       "avatar.asset.compatibility-enum",
                       "avatar.validation.compatibility");
    }
    if (hasDuplicates(draft.supportedRepresentations) ||
        hasDuplicates(draft.supportedRigFamilies) || hasDuplicates(draft.allowedSlots)) {
        return invalid("avatar asset compatibility declarations must be unique",
                       "avatar.asset.duplicate-compatibility",
                       "avatar.validation.duplicate-compatibility");
    }

    std::unordered_set<std::string> dependencyKeys;
    for (const auto& dependency : draft.dependencies) {
        if (!dependencyKeys.insert(dependency.assetId.value() + '\0' + dependency.version).second) {
            return invalid("avatar asset dependencies must be unique",
                           "avatar.asset.duplicate-dependency",
                           "avatar.validation.duplicate-dependency");
        }
    }
    std::unordered_set<std::string> paths;
    std::unordered_set<std::string> hashes;
    for (const auto& payload : draft.payloads) {
        if (!validPayloadPath(payload.path) || !validHash(payload.sha256)) {
            return invalid("avatar payload paths and SHA-256 hashes must be valid",
                           "avatar.asset.payload", "avatar.validation.payload");
        }
        if (!paths.insert(payload.path).second || !hashes.insert(payload.sha256).second) {
            return invalid("avatar payload paths and hashes must be unique",
                           "avatar.asset.duplicate-payload",
                           "avatar.validation.duplicate-payload");
        }
    }
    std::unordered_set<AvatarRight> rights;
    for (const auto& grant : draft.grants) {
        if (!enumInRange(grant.right, AvatarRight::CommercialBroadcast,
                         AvatarRight::Attribution) ||
            !enumInRange(grant.state, GrantState::Allowed,
                         GrantState::Unknown)) {
            return invalid("avatar license grant contains an unknown enum value",
                           "avatar.asset.grant-enum",
                           "avatar.validation.grant");
        }
        if (!rights.insert(grant.right).second) {
            return invalid("avatar license grants must name each right once",
                           "avatar.asset.duplicate-grant",
                           "avatar.validation.duplicate-grant");
        }
        if (!grant.condition.empty() && !utf8Length(grant.condition).has_value()) {
            return invalid("avatar license conditions must be valid UTF-8",
                           "avatar.asset.grant-condition",
                           "avatar.validation.grant-condition");
        }
    }
    const auto attribution = std::find_if(
        draft.grants.begin(), draft.grants.end(),
        [](const LicenseGrant& grant) { return grant.right == AvatarRight::Attribution; });
    const auto attributionLength = utf8Length(draft.attributionText);
    if (!attributionLength.has_value() || *attributionLength > 1000U) {
        return invalid("avatar attribution text must be valid UTF-8 and at most 1000 characters",
                       "avatar.asset.attribution", "avatar.validation.attribution");
    }
    if (attribution != draft.grants.end() && attribution->state == GrantState::Allowed &&
        draft.attributionText.empty()) {
        return invalid("allowed attribution requires non-empty attribution text",
                       "avatar.asset.attribution", "avatar.validation.attribution");
    }
    if (draft.validUntil.has_value() && *draft.validUntil <= draft.validFrom) {
        return invalid("avatar asset valid-until must be later than valid-from",
                       "avatar.asset.validity", "avatar.validation.validity");
    }
    for (const auto& region : draft.regionAllowList) {
        if (!validText(region, 64U)) {
            return invalid("avatar asset regions must be valid UTF-8",
                           "avatar.asset.region", "avatar.validation.region");
        }
    }
    if (hasDuplicates(draft.regionAllowList)) {
        return invalid("avatar asset regions must be unique",
                       "avatar.asset.duplicate-region", "avatar.validation.duplicate-region");
    }
    if (draft.assetId.value().starts_with("core.")) {
        for (const auto required : {AvatarRight::CommercialBroadcast, AvatarRight::AppBundle,
                                    AvatarRight::DerivativeCharacter}) {
            const auto grant = std::find_if(
                draft.grants.begin(), draft.grants.end(),
                [required](const LicenseGrant& candidate) { return candidate.right == required; });
            if (grant == draft.grants.end() || grant->state == GrantState::Conditional ||
                grant->state == GrantState::Unknown || !grant->condition.empty()) {
                return invalid("core avatar assets require explicit unconditional commercial grants",
                               "avatar.asset.core-grant",
                               "avatar.validation.core-grant");
            }
        }
    }
    return AvatarAssetManifest{std::move(draft)};
}

std::int32_t AvatarAssetManifest::schemaVersion() const noexcept {
    return kCurrentSchemaVersion;
}

const AvatarPackageId& AvatarAssetManifest::packageId() const noexcept {
    return values_.packageId;
}

const AvatarAssetId& AvatarAssetManifest::assetId() const noexcept {
    return values_.assetId;
}

const std::vector<LicenseGrant>& AvatarAssetManifest::grants() const noexcept {
    return values_.grants;
}

const std::string& AvatarAssetManifest::attributionText() const noexcept {
    return values_.attributionText;
}

const std::vector<std::string>& AvatarAssetManifest::regionAllowList() const noexcept {
    return values_.regionAllowList;
}

core::Utc AvatarAssetManifest::validFrom() const noexcept {
    return values_.validFrom;
}

const std::optional<core::Utc>& AvatarAssetManifest::validUntil() const noexcept {
    return values_.validUntil;
}

const AvatarAssetManifestDraft& AvatarAssetManifest::values() const noexcept {
    return values_;
}

}  // namespace creator::avatar
