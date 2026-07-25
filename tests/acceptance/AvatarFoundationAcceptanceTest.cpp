#include "avatar/AvatarLicenseResolver.h"
#include "avatar/AvatarSpec.h"
#include "avatar/AvatarSpecCodec.h"
#include "avatar_pack_adapter/FileAvatarCatalog.h"
#include "avatar_pack_adapter/SignedAvatarPackFixture.h"
#include "project_store/AvatarSpecFileStore.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

using creator::avatar::AssetRef;
using creator::avatar::AvatarAssetId;
using creator::avatar::AvatarId;
using creator::avatar::AvatarLicenseResolver;
using creator::avatar::AvatarRepresentation;
using creator::avatar::AvatarRight;
using creator::avatar::AvatarRightsSnapshot;
using creator::avatar::AvatarRightsSnapshotEntry;
using creator::avatar::AvatarSlot;
using creator::avatar::AvatarSpec;
using creator::avatar::AvatarSpecCodec;
using creator::avatar::AvatarSpecDraft;
using creator::avatar::AvatarUseContext;
using creator::avatar::GrantState;
using creator::avatar::LicenseGrant;
using creator::avatar::RigFamily;
using creator::avatar::UseKind;
using creator::avatar::UserKind;
using creator::avatar_pack_adapter::FileAvatarCatalog;
using creator::avatar_pack_adapter::TrustedAvatarKey;
using creator::avatar_pack_adapter::test::SignedAvatarPackFixture;
using creator::avatar_pack_adapter::test::SignedPackOptions;
using creator::avatar_pack_adapter::test::SignedPackPayload;
using creator::project_store::AvatarSpecFileStore;

// This deterministic seed is test evidence, not a Creator Studio release key.
// It is compiled only into cs_avatar_foundation_acceptance_tests.
constexpr std::array<std::uint8_t, crypto_sign_SEEDBYTES>
    kAcceptanceSigningSeed{
        0x43U, 0x53U, 0x2dU, 0x41U, 0x56U, 0x41U, 0x54U, 0x41U,
        0x52U, 0x2dU, 0x46U, 0x4fU, 0x55U, 0x4eU, 0x44U, 0x41U,
        0x54U, 0x49U, 0x4fU, 0x4eU, 0x2dU, 0x54U, 0x45U, 0x53U,
        0x54U, 0x2dU, 0x4bU, 0x45U, 0x59U, 0x2dU, 0x30U, 0x31U,
    };

std::vector<std::uint8_t> readBytes(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) return {};
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

void writeBytes(const fs::path& path, std::string_view contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
    if (!output) {
        throw std::runtime_error{"acceptance source payload write failed"};
    }
}

class TestSignedPackFactory final {
public:
    explicit TestSignedPackFactory(const fs::path& workspace)
        : sourcePayload_(workspace / "source" / "model.bin"),
          fixture_(workspace / "packs", kAcceptanceSigningSeed) {}

    fs::path createCommercialPack() {
        writeBytes(sourcePayload_,
                   "runtime-generated-commercial-avatar-payload-v1");
        SignedPackOptions options;
        options.payloads = {
            SignedPackPayload{.path = "payload/model.bin",
                              .contents = readBytes(sourcePayload_)},
        };
        return fixture_.writePack(std::move(options));
    }

    [[nodiscard]] TrustedAvatarKey trustedKey() const {
        return fixture_.trustedKey();
    }

    [[nodiscard]] const fs::path& sourcePayload() const noexcept {
        return sourcePayload_;
    }

private:
    fs::path sourcePayload_;
    SignedAvatarPackFixture fixture_;
};

AvatarSpec specUsingInstalledAsset() {
    const auto assetId =
        AvatarAssetId::create("core.body.humanoid").value();
    const AssetRef installed{assetId, "1.0.0", "default"};
    AvatarSpecDraft draft{
        .avatarId = AvatarId::create("avatar-acceptance").value(),
        .displayName = "Commercial Acceptance Avatar",
        .rigFamily = RigFamily::Humanoid,
        .speciesFamily = "human",
        .styleTheme = "commercial-studio",
        .preferredRepresentation = AvatarRepresentation::GltfRig,
        .bodyMorphs = {{"height", 0.1F}},
        .faceMorphs = {{"eye-width", -0.1F}},
        .animalMorphs = {},
        .slots = {
            {AvatarSlot::Body, installed},
            {AvatarSlot::Head, installed},
            {AvatarSlot::Eyes, installed},
            {AvatarSlot::Mouth, installed},
        },
        .palette = {},
        .materials = {},
        .expressions = {{"happy", 0.5F}},
        .physics = {},
        .trackingProfileId = "acceptance-tracking",
    };
    return AvatarSpec::create(std::move(draft)).value();
}

AvatarUseContext corporateMonetizedBroadcast() {
    return {
        .userKind = UserKind::Corporation,
        .useKind = UseKind::Broadcast,
        .monetized = true,
        .region = "KR",
        .evaluatedAt =
            creator::core::Utc::parseRfc3339("2026-07-24T00:00:00Z")
                .value(),
    };
}

class AvatarFoundationAcceptanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        root_ = fs::temp_directory_path() /
                ("creator-avatar-foundation-" +
                 std::string{info->name()});
        std::error_code ignored;
        fs::remove_all(root_, ignored);
        ASSERT_TRUE(fs::create_directories(root_));
#ifndef _WIN32
        fs::permissions(root_, fs::perms::owner_all,
                        fs::perm_options::replace);
#endif
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    FileAvatarCatalog openCatalog(const TestSignedPackFactory& factory) {
        auto opened = FileAvatarCatalog::open(
            root_ / "catalog", {factory.trustedKey()});
        if (!opened.hasValue()) {
            throw std::runtime_error{opened.error().message()};
        }
        return std::move(opened).value();
    }

    fs::path root_;
};

TEST_F(AvatarFoundationAcceptanceTest,
       SignedCommercialAssetSurvivesProjectRoundTrip) {
    TestSignedPackFactory factory{root_ / "factory"};
    TestSignedPackFactory duplicateFactory{root_ / "duplicate-factory"};
    const auto pack = factory.createCommercialPack();
    const auto duplicatePack = duplicateFactory.createCommercialPack();
    ASSERT_TRUE(fs::is_regular_file(pack));
    ASSERT_TRUE(fs::is_regular_file(duplicatePack));
    ASSERT_FALSE(readBytes(pack).empty());
    ASSERT_FALSE(readBytes(duplicatePack).empty());
    ASSERT_FALSE(readBytes(factory.sourcePayload()).empty());
    ASSERT_EQ(readBytes(pack), readBytes(duplicatePack))
        << "same runtime source and deterministic test seed must reproduce "
           "the signed archive";

    auto catalog = openCatalog(factory);
    const auto installed = catalog.install(pack);
    ASSERT_TRUE(installed.hasValue()) << installed.error().message();

    ASSERT_TRUE(fs::create_directories(root_ / "project" / "avatars"));
    AvatarSpecFileStore specs{root_ / "project" / "avatars"};
    const auto spec = specUsingInstalledAsset();
    const auto saved = specs.save(spec);
    ASSERT_TRUE(saved.hasValue()) << saved.error().message();
    const auto reopened = specs.load(spec.avatarId());
    ASSERT_TRUE(reopened.hasValue()) << reopened.error().message();
    EXPECT_EQ(AvatarSpecCodec{}.toJson(reopened.value()),
              AvatarSpecCodec{}.toJson(spec));

    const auto assetId =
        AvatarAssetId::create("core.body.humanoid").value();
    const auto manifest = catalog.find(assetId, "1.0.0");
    ASSERT_TRUE(manifest.hasValue()) << manifest.error().message();
    const std::vector currentManifests{manifest.value()};
    const auto rights = AvatarLicenseResolver{}.resolve(
        corporateMonetizedBroadcast(), currentManifests);
    EXPECT_TRUE(rights.allowed);
    EXPECT_TRUE(rights.blockers.empty());

    const auto payloadRoot = catalog.payloadRoot(assetId, "1.0.0");
    ASSERT_TRUE(payloadRoot.hasValue()) << payloadRoot.error().message();
    EXPECT_EQ(readBytes(payloadRoot.value() / "model.bin"),
              readBytes(factory.sourcePayload()));
    writeBytes(payloadRoot.value() / "model.bin", "tampered");

    const auto rejected = catalog.payloadRoot(assetId, "1.0.0");
    ASSERT_FALSE(rejected.hasValue());
}

TEST_F(AvatarFoundationAcceptanceTest,
       CurrentManifestBlocksRightsDespiteOlderAllowedEvidence) {
    TestSignedPackFactory factory{root_ / "factory"};
    auto catalog = openCatalog(factory);
    ASSERT_TRUE(catalog.install(factory.createCommercialPack()).hasValue());
    const auto assetId =
        AvatarAssetId::create("core.body.humanoid").value();
    const auto manifest = catalog.find(assetId, "1.0.0");
    ASSERT_TRUE(manifest.hasValue()) << manifest.error().message();

    auto context = corporateMonetizedBroadcast();
    context.useKind = UseKind::ModelExport;
    context.recordedSnapshot = AvatarRightsSnapshot{
        .entries = {
            AvatarRightsSnapshotEntry{
                .assetId = assetId,
                .recordedGrants = {
                    LicenseGrant{AvatarRight::ModelExport,
                                 GrantState::Allowed, ""},
                },
            },
        },
        .recordedAt =
            creator::core::Utc::parseRfc3339("2026-07-23T00:00:00Z")
                .value(),
        .evaluatedAt =
            creator::core::Utc::parseRfc3339("2026-07-23T00:00:00Z")
                .value(),
    };
    const std::vector currentManifests{manifest.value()};

    const auto rights =
        AvatarLicenseResolver{}.resolve(context, currentManifests);

    ASSERT_FALSE(rights.allowed);
    ASSERT_EQ(rights.blockers.size(), 1U);
    EXPECT_EQ(rights.blockers.front().assetId.value(),
              "core.body.humanoid");
    EXPECT_EQ(rights.blockers.front().right, AvatarRight::ModelExport);
    EXPECT_EQ(rights.blockers.front().state, GrantState::Unknown);
}

bool containsAcceptanceSeed(const fs::path& binary) {
    const auto contents = readBytes(binary);
    if (contents.empty()) {
        throw std::runtime_error{"artifact scan could not read: " +
                                 binary.string()};
    }
    return std::search(contents.begin(), contents.end(),
                       kAcceptanceSigningSeed.begin(),
                       kAcceptanceSigningSeed.end()) != contents.end();
}

TEST(AvatarFoundationAcceptanceKeyIsolationTest,
     TestSigningSeedDoesNotAppearInShippedArtifacts) {
    const fs::path acceptanceBinary{CS_ACCEPTANCE_BINARY_PATH};
    ASSERT_TRUE(fs::is_regular_file(acceptanceBinary));
    ASSERT_TRUE(containsAcceptanceSeed(acceptanceBinary))
        << "the isolation scan is vacuous unless it can find the test seed "
           "in the acceptance binary";

    const std::array shippedArtifacts{
        fs::path{CS_SHIPPED_CREATOR_STUDIO_PATH},
        fs::path{CS_SHIPPED_AVATAR_PACK_ADAPTER_PATH},
        fs::path{CS_SHIPPED_AVATAR_LIBRARY_PATH},
        fs::path{CS_SHIPPED_PROJECT_STORE_PATH},
    };
    for (const auto& artifact : shippedArtifacts) {
        SCOPED_TRACE(artifact.string());
        ASSERT_TRUE(fs::is_regular_file(artifact));
        EXPECT_FALSE(containsAcceptanceSeed(artifact));
    }
}

}  // namespace
