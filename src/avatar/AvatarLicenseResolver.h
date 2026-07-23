#pragma once

#include "avatar/AvatarLicense.h"

#include <span>
#include <string_view>

namespace creator::avatar {

class AvatarAssetManifest;

/// Resolves every authorization from current manifests, never recorded snapshots.
class AvatarLicenseResolver final {
public:
    [[nodiscard]] AvatarRightsDecision resolve(
        const AvatarUseContext& context,
        std::span<const AvatarAssetManifest> manifests) const;
    [[nodiscard]] AvatarRightsMatrix resolveAll(
        UserKind userKind,
        bool monetized,
        std::string_view region,
        core::Utc evaluatedAt,
        std::span<const AvatarAssetManifest> manifests) const;
};

}  // namespace creator::avatar
