#include "avatar/AvatarLicenseResolver.h"

#include "avatar/AvatarAssetManifest.h"

#include <algorithm>
#include <array>
#include <string>
#include <tuple>
#include <vector>

namespace creator::avatar {
namespace {

struct UseRequirement final {
    bool recognized;
    std::vector<AvatarRight> rights;
};

UseRequirement requiredRights(UseKind use) {
    switch (use) {
    case UseKind::Preview: return {true, {}};
    case UseKind::Broadcast:
    case UseKind::Record:
        return {true, {AvatarRight::CommercialBroadcast}};
    case UseKind::AppBundle: return {true, {AvatarRight::AppBundle}};
    case UseKind::DerivativeCharacter:
        return {true, {AvatarRight::DerivativeCharacter}};
    case UseKind::ModelExport: return {true, {AvatarRight::ModelExport}};
    case UseKind::PortableProject:
        return {true, {AvatarRight::PortableProject}};
    }
    return {false, {}};
}

const LicenseGrant* findGrant(const AvatarAssetManifest& manifest, AvatarRight right) {
    const auto& grants = manifest.grants();
    const auto found = std::find_if(grants.begin(), grants.end(),
                                    [right](const LicenseGrant& grant) {
                                        return grant.right == right;
                                    });
    return found == grants.end() ? nullptr : &*found;
}

std::string grantReason(const LicenseGrant* grant) {
    if (grant == nullptr) return "avatar.rights.missing-grant";
    if (!grant->condition.empty()) {
        return "avatar.rights.condition: " + grant->condition;
    }
    switch (grant->state) {
    case GrantState::Allowed: return {};
    case GrantState::Denied: return "avatar.rights.denied";
    case GrantState::Conditional: return "avatar.rights.conditional";
    case GrantState::Unknown: return "avatar.rights.unknown";
    }
    return "avatar.rights.unknown";
}

}  // namespace

AvatarRightsDecision AvatarLicenseResolver::resolve(
    const AvatarUseContext& context,
    std::span<const AvatarAssetManifest> manifests) const {
    std::vector<const AvatarAssetManifest*> ordered;
    ordered.reserve(manifests.size());
    for (const auto& manifest : manifests) ordered.push_back(&manifest);
    std::sort(ordered.begin(), ordered.end(),
              [](const AvatarAssetManifest* left, const AvatarAssetManifest* right) {
                  return left->assetId().value() < right->assetId().value();
              });

    AvatarRightsDecision decision;
    const auto requirement = requiredRights(context.useKind);
    if (!requirement.recognized) return decision;
    if (!requirement.rights.empty() && manifests.empty()) return decision;

    const auto& rights = requirement.rights;
    for (const auto* manifest : ordered) {
        const auto& regions = manifest->regionAllowList();
        const bool regionAllowed =
            regions.empty() || std::find(regions.begin(), regions.end(), context.region) != regions.end();
        const bool timeAllowed =
            context.evaluatedAt >= manifest->validFrom() &&
            (!manifest->validUntil().has_value() ||
             context.evaluatedAt < *manifest->validUntil());
        if (!regionAllowed) {
            for (const auto right : rights) {
                decision.blockers.push_back(
                    {manifest->assetId(), right, GrantState::Denied,
                     "avatar.rights.region: " + context.region});
            }
        }
        if (!timeAllowed) {
            for (const auto right : rights) {
                decision.blockers.push_back(
                    {manifest->assetId(), right, GrantState::Denied,
                     "avatar.rights.time"});
            }
        }
        for (const auto right : rights) {
            const auto* grant = findGrant(*manifest, right);
            const auto reason = grantReason(grant);
            if (!reason.empty()) {
                decision.blockers.push_back(
                    {manifest->assetId(), right,
                     grant == nullptr ? GrantState::Unknown : grant->state, reason});
            }
        }
        const auto* attribution = findGrant(*manifest, AvatarRight::Attribution);
        if (attribution != nullptr && attribution->state == GrantState::Allowed &&
            attribution->condition.empty()) {
            decision.attributionRequired = true;
            decision.attributionLines.push_back(manifest->attributionText());
        }
    }
    std::sort(decision.blockers.begin(), decision.blockers.end(),
              [](const AvatarRightBlocker& left,
                 const AvatarRightBlocker& right) {
                  return std::tie(left.assetId.value(), left.right, left.reason,
                                  left.state) <
                         std::tie(right.assetId.value(), right.right,
                                  right.reason, right.state);
              });
    decision.allowed = decision.blockers.empty();
    return decision;
}

AvatarRightsMatrix AvatarLicenseResolver::resolveAll(
    UserKind userKind,
    bool monetized,
    std::string_view region,
    core::Utc evaluatedAt,
    std::span<const AvatarAssetManifest> manifests) const {
    AvatarRightsMatrix matrix;
    constexpr std::array uses{
        UseKind::Preview, UseKind::Broadcast, UseKind::Record, UseKind::AppBundle,
        UseKind::DerivativeCharacter, UseKind::ModelExport, UseKind::PortableProject};
    for (const auto use : uses) {
        matrix.byUse.emplace(
            use, resolve({.userKind = userKind,
                          .useKind = use,
                          .monetized = monetized,
                          .region = std::string{region},
                          .evaluatedAt = evaluatedAt},
                         manifests));
    }
    return matrix;
}

}  // namespace creator::avatar
