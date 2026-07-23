#pragma once

#include "avatar/AvatarIdentifiers.h"
#include "core/Utc.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace creator::avatar {

enum class AvatarRight {
    CommercialBroadcast,
    AppBundle,
    DerivativeCharacter,
    ModelExport,
    RawAssetRedistribution,
    PortableProject,
    Attribution
};

enum class GrantState { Allowed, Denied, Conditional, Unknown };
enum class UserKind { Individual, Corporation };
enum class UseKind {
    Preview,
    Broadcast,
    Record,
    AppBundle,
    DerivativeCharacter,
    ModelExport,
    PortableProject
};

/// One explicit right recorded by an asset vendor.
struct LicenseGrant final {
    AvatarRight right;
    GrantState state{GrantState::Unknown};
    std::string condition;

    friend bool operator==(const LicenseGrant&, const LicenseGrant&) = default;
};

/// Audit-only copy of the grants recorded for one asset.
struct AvatarRightsSnapshotEntry final {
    AvatarAssetId assetId;
    std::vector<LicenseGrant> recordedGrants;
};

/// Audit provenance retained with an output; never an authorization source.
struct AvatarRightsSnapshot final {
    std::vector<AvatarRightsSnapshotEntry> entries;
    core::Utc recordedAt;
    core::Utc evaluatedAt;
};

/// Current use being authorized against current asset manifests.
struct AvatarUseContext final {
    UserKind userKind{UserKind::Individual};
    UseKind useKind{UseKind::Preview};
    bool monetized{false};
    std::string region;
    core::Utc evaluatedAt;
    std::optional<AvatarRightsSnapshot> recordedSnapshot;
};

struct AvatarRightBlocker final {
    AvatarAssetId assetId;
    AvatarRight right;
    GrantState state;
    std::string reason;
};

struct AvatarRightsDecision final {
    bool allowed{false};
    bool attributionRequired{false};
    std::vector<std::string> attributionLines;
    std::vector<AvatarRightBlocker> blockers;
};

/// Editor-only view of independently evaluated current decisions.
struct AvatarRightsMatrix final {
    std::map<UseKind, AvatarRightsDecision> byUse;

    [[nodiscard]] const AvatarRightsDecision& forUse(UseKind use) const;
};

}  // namespace creator::avatar
