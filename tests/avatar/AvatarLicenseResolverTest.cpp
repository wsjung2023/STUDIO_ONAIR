#include "avatar/AvatarLicenseResolver.h"

#include "avatar/AvatarAssetManifest.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace creator::avatar;
using creator::core::Utc;

Utc utc(std::string_view value) {
    return Utc::parseRfc3339(value).value();
}

std::vector<LicenseGrant> allowAll() {
    return {
        {AvatarRight::CommercialBroadcast, GrantState::Allowed, {}},
        {AvatarRight::AppBundle, GrantState::Allowed, {}},
        {AvatarRight::DerivativeCharacter, GrantState::Allowed, {}},
        {AvatarRight::ModelExport, GrantState::Allowed, {}},
        {AvatarRight::RawAssetRedistribution, GrantState::Allowed, {}},
        {AvatarRight::PortableProject, GrantState::Allowed, {}},
    };
}

std::vector<LicenseGrant> withGrant(AvatarRight right, GrantState state) {
    auto grants = allowAll();
    for (auto& grant : grants) {
        if (grant.right == right) {
            grant.state = state;
            grant.condition =
                state == GrantState::Conditional ? "vendor approval required" : "";
        }
    }
    return grants;
}

AvatarAssetManifest manifest(std::string id, std::vector<LicenseGrant> grants) {
    AvatarAssetManifestDraft draft{
        .packageId = AvatarPackageId::create("core.foundation").value(),
        .packageVersion = "1.0.0",
        .assetId = AvatarAssetId::create(std::move(id)).value(),
        .assetVersion = "1.0.0",
        .displayName = "Foundation asset",
        .vendor = "Creator Studio",
        .supportedRepresentations = {AvatarRepresentation::Inochi2d},
        .supportedRigFamilies = {RigFamily::Humanoid},
        .allowedSlots = {AvatarSlot::Body},
        .dependencies = {},
        .payloads = {{"payload/body.json",
                      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"}},
        // Zero metadata means not applicable for this minimal rights fixture.
        .performance = {},
        .sourceUri = "https://creator-studio.local/assets/body",
        .licenseId = "creator-studio-core",
        .licenseVersion = "1.0.0",
        .grants = std::move(grants),
        .attributionText = {},
        .regionAllowList = {},
        .validFrom = utc("2026-01-01T00:00:00Z"),
        .validUntil = std::nullopt,
    };
    return AvatarAssetManifest::create(std::move(draft)).value();
}

AvatarUseContext commercialCorporateExport() {
    return {
        .userKind = UserKind::Corporation,
        .useKind = UseKind::ModelExport,
        .monetized = true,
        .region = "KR",
        .evaluatedAt = utc("2026-07-24T00:00:00Z"),
    };
}

AvatarUseContext commercialBroadcast() {
    return {
        .userKind = UserKind::Corporation,
        .useKind = UseKind::Broadcast,
        .monetized = true,
        .region = "KR",
        .evaluatedAt = utc("2026-07-24T00:00:00Z"),
    };
}

AvatarRightsSnapshot snapshotAllowingAll() {
    return {
        .entries = {{.assetId = AvatarAssetId::create("core.body").value(),
                     .recordedGrants = allowAll()}},
        .recordedAt = utc("2026-07-23T00:00:00Z"),
        .evaluatedAt = utc("2026-07-23T00:00:00Z"),
    };
}

TEST(AvatarLicenseResolverTest, NamesEveryAssetBlockingModelExport) {
    const std::vector manifests{
        manifest("core.body", allowAll()),
        manifest("core.hair",
                 withGrant(AvatarRight::ModelExport, GrantState::Denied)),
    };
    const auto result =
        AvatarLicenseResolver{}.resolve(commercialCorporateExport(), manifests);
    EXPECT_FALSE(result.allowed);
    ASSERT_EQ(result.blockers.size(), 1U);
    EXPECT_EQ(result.blockers[0].assetId.value(), "core.hair");
    EXPECT_EQ(result.blockers[0].right, AvatarRight::ModelExport);
}

TEST(AvatarLicenseResolverTest, SnapshotNeverOverridesCurrentManifest) {
    auto context = commercialBroadcast();
    context.recordedSnapshot = snapshotAllowingAll();
    const std::vector manifests{
        manifest("core.body",
                 withGrant(AvatarRight::CommercialBroadcast, GrantState::Denied)),
    };
    const auto result = AvatarLicenseResolver{}.resolve(context, manifests);
    EXPECT_FALSE(result.allowed);
}

TEST(AvatarLicenseResolverTest, ReturnsEveryBlockerInAssetAndRightOrder) {
    auto unknownAndDenied = allowAll();
    for (auto& grant : unknownAndDenied) {
        if (grant.right == AvatarRight::RawAssetRedistribution) {
            grant.state = GrantState::Unknown;
        }
        if (grant.right == AvatarRight::PortableProject) {
            grant.state = GrantState::Denied;
        }
    }
    const std::vector manifests{
        manifest("vendor.zeta", unknownAndDenied),
        manifest("vendor.alpha", unknownAndDenied),
    };
    auto context = commercialCorporateExport();
    context.useKind = UseKind::PortableProject;
    const auto result = AvatarLicenseResolver{}.resolve(context, manifests);
    ASSERT_EQ(result.blockers.size(), 2U);
    EXPECT_EQ(result.blockers[0].assetId.value(), "vendor.alpha");
    EXPECT_EQ(result.blockers[0].right, AvatarRight::PortableProject);
    EXPECT_EQ(result.blockers[1].assetId.value(), "vendor.zeta");
    EXPECT_EQ(result.blockers[1].right, AvatarRight::PortableProject);
}

TEST(AvatarLicenseResolverTest, MissingAndConditionalGrantsFailClosedWithReasons) {
    auto conditional = allowAll();
    conditional.erase(
        std::remove_if(conditional.begin(), conditional.end(),
                       [](const LicenseGrant& grant) {
                           return grant.right == AvatarRight::ModelExport;
                       }),
        conditional.end());
    const std::vector missing{manifest("vendor.missing", conditional)};
    auto result =
        AvatarLicenseResolver{}.resolve(commercialCorporateExport(), missing);
    ASSERT_EQ(result.blockers.size(), 1U);
    EXPECT_EQ(result.blockers[0].state, GrantState::Unknown);
    EXPECT_NE(result.blockers[0].reason.find("avatar.rights.missing-grant"),
              std::string::npos);

    conditional.push_back({AvatarRight::ModelExport, GrantState::Conditional,
                           "written vendor approval"});
    const std::vector conditioned{manifest("vendor.conditional", conditional)};
    result = AvatarLicenseResolver{}.resolve(commercialCorporateExport(),
                                              conditioned);
    ASSERT_EQ(result.blockers.size(), 1U);
    EXPECT_NE(result.blockers[0].reason.find("written vendor approval"),
              std::string::npos);
}

TEST(AvatarLicenseResolverTest, RegionAndTimeConditionsAreBothReported) {
    auto restricted = manifest("vendor.restricted", allowAll());
    auto draft = restricted.values();
    draft.regionAllowList = {"US"};
    draft.validFrom = utc("2027-01-01T00:00:00Z");
    const std::vector manifests{
        AvatarAssetManifest::create(std::move(draft)).value()};
    const auto result =
        AvatarLicenseResolver{}.resolve(commercialCorporateExport(), manifests);
    ASSERT_EQ(result.blockers.size(), 2U);
    EXPECT_NE(result.blockers[0].reason.find("avatar.rights.region"),
              std::string::npos);
    EXPECT_NE(result.blockers[1].reason.find("avatar.rights.time"),
              std::string::npos);
}

TEST(AvatarLicenseResolverTest, CanonicallyOrdersPolicyAndGrantBlockers) {
    auto grants = allowAll();
    grants.erase(std::remove_if(
                     grants.begin(), grants.end(),
                     [](const LicenseGrant& grant) {
                         return grant.right == AvatarRight::RawAssetRedistribution ||
                                grant.right == AvatarRight::PortableProject;
                     }),
                 grants.end());
    auto draft = manifest("vendor.restricted", grants).values();
    draft.regionAllowList = {"US"};
    draft.validFrom = utc("2027-01-01T00:00:00Z");
    const std::vector manifests{
        AvatarAssetManifest::create(std::move(draft)).value()};
    auto context = commercialCorporateExport();
    context.useKind = UseKind::PortableProject;
    const auto result = AvatarLicenseResolver{}.resolve(context, manifests);
    ASSERT_EQ(result.blockers.size(), 3U);
    for (std::size_t index = 0; index < 3U; ++index) {
        EXPECT_EQ(result.blockers[index].right, AvatarRight::PortableProject);
    }
    EXPECT_LT(result.blockers[0].reason, result.blockers[1].reason);
    EXPECT_LT(result.blockers[1].reason, result.blockers[2].reason);
}

TEST(AvatarLicenseResolverTest, PreviewIgnoresRightsRegionAndValidityGates) {
    auto draft = manifest(
                     "vendor.preview",
                     withGrant(AvatarRight::CommercialBroadcast,
                               GrantState::Unknown))
                     .values();
    draft.regionAllowList = {"US"};
    draft.validFrom = utc("2027-01-01T00:00:00Z");
    const std::vector manifests{
        AvatarAssetManifest::create(std::move(draft)).value()};
    auto context = commercialBroadcast();
    context.useKind = UseKind::Preview;
    const auto result = AvatarLicenseResolver{}.resolve(context, manifests);
    EXPECT_TRUE(result.allowed);
    EXPECT_TRUE(result.blockers.empty());
}

TEST(AvatarLicenseResolverTest, EmptyManifestSetFailsClosedExceptForPreview) {
    const std::vector<AvatarAssetManifest> manifests;
    auto context = commercialCorporateExport();
    const auto denied = AvatarLicenseResolver{}.resolve(context, manifests);
    EXPECT_FALSE(denied.allowed);
    EXPECT_TRUE(denied.blockers.empty());

    context.useKind = UseKind::Preview;
    const auto preview = AvatarLicenseResolver{}.resolve(context, manifests);
    EXPECT_TRUE(preview.allowed);
    EXPECT_TRUE(preview.blockers.empty());
}

TEST(AvatarLicenseResolverTest, OutOfDomainUseKindFailsClosed) {
    const std::vector manifests{manifest("vendor.body", allowAll())};
    auto context = commercialCorporateExport();
    context.useKind = static_cast<UseKind>(999);
    const auto result = AvatarLicenseResolver{}.resolve(context, manifests);
    EXPECT_FALSE(result.allowed);
    EXPECT_TRUE(result.blockers.empty());
}

TEST(AvatarLicenseResolverTest, ValidityWindowIsStartInclusiveAndEndExclusive) {
    auto draft = manifest("vendor.windowed", allowAll()).values();
    draft.validFrom = utc("2026-07-24T00:00:00Z");
    draft.validUntil = utc("2026-07-25T00:00:00Z");
    const std::vector manifests{
        AvatarAssetManifest::create(std::move(draft)).value()};
    auto context = commercialCorporateExport();

    context.evaluatedAt = utc("2026-07-24T00:00:00Z");
    EXPECT_TRUE(AvatarLicenseResolver{}.resolve(context, manifests).allowed);

    context.evaluatedAt = utc("2026-07-25T00:00:00Z");
    const auto expired = AvatarLicenseResolver{}.resolve(context, manifests);
    EXPECT_FALSE(expired.allowed);
    ASSERT_EQ(expired.blockers.size(), 1U);
    EXPECT_NE(expired.blockers[0].reason.find("avatar.rights.time"),
              std::string::npos);
}

TEST(AvatarLicenseResolverTest, PortableProjectDoesNotImplyRawRedistribution) {
    auto grants = withGrant(AvatarRight::RawAssetRedistribution,
                            GrantState::Denied);
    const std::vector manifests{manifest("vendor.portable", grants)};
    auto context = commercialCorporateExport();
    context.useKind = UseKind::PortableProject;
    const auto result = AvatarLicenseResolver{}.resolve(context, manifests);
    EXPECT_TRUE(result.allowed);
}

TEST(AvatarLicenseResolverTest, AttributionIsReturnedWithoutBecomingAnAuthorizationCache) {
    auto draft = manifest("vendor.attributed", allowAll()).values();
    draft.grants.push_back(
        {AvatarRight::Attribution, GrantState::Allowed, {}});
    draft.attributionText = "Hair by Vendor";
    const std::vector manifests{
        AvatarAssetManifest::create(std::move(draft)).value()};
    const auto result =
        AvatarLicenseResolver{}.resolve(commercialCorporateExport(), manifests);
    EXPECT_TRUE(result.allowed);
    EXPECT_TRUE(result.attributionRequired);
    ASSERT_EQ(result.attributionLines.size(), 1U);
    EXPECT_EQ(result.attributionLines[0], "Hair by Vendor");
}

TEST(AvatarLicenseResolverTest, ResolveAllEvaluatesEveryUseKind) {
    const std::vector manifests{manifest("core.body", allowAll())};
    const auto matrix = AvatarLicenseResolver{}.resolveAll(
        UserKind::Individual, true, "KR", utc("2026-07-24T00:00:00Z"),
        manifests);
    EXPECT_EQ(matrix.byUse.size(), 7U);
    EXPECT_TRUE(matrix.forUse(UseKind::Preview).allowed);
    EXPECT_TRUE(matrix.forUse(UseKind::Broadcast).allowed);
    EXPECT_TRUE(matrix.forUse(UseKind::Record).allowed);
    EXPECT_TRUE(matrix.forUse(UseKind::AppBundle).allowed);
    EXPECT_TRUE(matrix.forUse(UseKind::DerivativeCharacter).allowed);
    EXPECT_TRUE(matrix.forUse(UseKind::ModelExport).allowed);
    EXPECT_TRUE(matrix.forUse(UseKind::PortableProject).allowed);
}

TEST(AvatarLicenseResolverTest, CommercialBroadcastBindsEveryUserAndMonetizationContext) {
    const std::vector manifests{
        manifest("core.body",
                 withGrant(AvatarRight::CommercialBroadcast,
                           GrantState::Denied))};
    for (const auto user : {UserKind::Individual, UserKind::Corporation}) {
        for (const bool monetized : {false, true}) {
            for (const auto use : {UseKind::Broadcast, UseKind::Record}) {
                auto context = commercialBroadcast();
                context.userKind = user;
                context.useKind = use;
                context.monetized = monetized;
                const auto result =
                    AvatarLicenseResolver{}.resolve(context, manifests);
                ASSERT_FALSE(result.allowed);
                ASSERT_EQ(result.blockers.size(), 1U);
                EXPECT_EQ(result.blockers[0].right,
                          AvatarRight::CommercialBroadcast);
            }
        }
    }
}

}  // namespace
