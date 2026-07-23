#include "avatar/AvatarAssetManifestCodec.h"

#include <gtest/gtest.h>

#include <nlohmann/json-schema.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

namespace {

using namespace creator::avatar;
using creator::core::ErrorCode;
using creator::core::Utc;
using nlohmann::json;

Utc utc(std::string_view value) {
    return Utc::parseRfc3339(value).value();
}

std::vector<LicenseGrant> grants() {
    return {
        {AvatarRight::CommercialBroadcast, GrantState::Allowed, {}},
        {AvatarRight::AppBundle, GrantState::Allowed, {}},
        {AvatarRight::DerivativeCharacter, GrantState::Denied, {}},
        {AvatarRight::ModelExport, GrantState::Conditional, "vendor approval"},
        {AvatarRight::RawAssetRedistribution, GrantState::Denied, {}},
        {AvatarRight::PortableProject, GrantState::Allowed, {}},
        {AvatarRight::Attribution, GrantState::Allowed, {}},
    };
}

AvatarAssetManifestDraft validDraft() {
    return {
        .packageId = AvatarPackageId::create("vendor.foundation").value(),
        .packageVersion = "2.1.0",
        .assetId = AvatarAssetId::create("vendor.body.base").value(),
        .assetVersion = "3.2.1",
        .displayName = "기본 바디",
        .vendor = "Creator Studio",
        .supportedRepresentations = {AvatarRepresentation::Vrm1,
                                     AvatarRepresentation::Inochi2d},
        .supportedRigFamilies = {RigFamily::Kemonomimi, RigFamily::Humanoid},
        .allowedSlots = {AvatarSlot::Head, AvatarSlot::Body},
        .dependencies = {
            {AvatarAssetId::create("vendor.rig.base").value(), "1.2.0"},
            {AvatarAssetId::create("vendor.material.base").value(), "1.0.0"},
        },
        .payloads = {
            {"textures/body.png",
             "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"},
            {"models/body.vrm",
             "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},
        },
        .performance = {.payloadBytes = 8388608,
                        .textureBytes = 4194304,
                        .textureCount = 4,
                        .maxTextureDimension = 2048,
                        .vertexCount = 12000,
                        .triangleCount = 8000,
                        .drawCallCount = 6,
                        .drawPartCount = 0,
                        .boneCount = 72},
        .sourceUri = "https://creator-studio.local/assets/body",
        .licenseId = "vendor-commercial",
        .licenseVersion = "4.0.0",
        .grants = grants(),
        .attributionText = "Body by Creator Studio",
        .regionAllowList = {"US", "KR"},
        .validFrom = utc("2026-01-01T00:00:00Z"),
        .validUntil = utc("2027-01-01T00:00:00Z"),
    };
}

AvatarAssetManifest validManifest() {
    return AvatarAssetManifest::create(validDraft()).value();
}

TEST(AvatarAssetManifestTest, CanonicalRoundTripSortsEverySetLikeCollection) {
    const auto document = AvatarAssetManifestCodec{}.toJson(validManifest());
    const auto decoded = AvatarAssetManifestCodec{}.fromJson(document);
    ASSERT_TRUE(decoded.hasValue()) << decoded.error().message();
    EXPECT_EQ(AvatarAssetManifestCodec{}.toJson(decoded.value()), document);
    EXPECT_EQ(document["supportedRepresentations"][0], "inochi2d");
    EXPECT_EQ(document["supportedRigFamilies"][0], "humanoid");
    EXPECT_EQ(document["allowedSlots"][0], "body");
    EXPECT_EQ(document["dependencies"][0]["assetId"], "vendor.material.base");
    EXPECT_EQ(document["payloads"][0]["path"], "models/body.vrm");
    EXPECT_EQ(document["grants"][0]["right"], "commercial-broadcast");
    EXPECT_EQ(document["regionAllowList"][0], "KR");
    EXPECT_EQ(document["performance"]["payloadBytes"], 8388608);
    EXPECT_EQ(document["performance"]["textureCount"], 4);
    EXPECT_EQ(document["performance"]["maxTextureDimension"], 2048);
    EXPECT_EQ(document["performance"]["drawCallCount"], 6);
    EXPECT_EQ(document["performance"]["drawPartCount"], 0);
    EXPECT_EQ(document["performance"]["boneCount"], 72);
}

TEST(AvatarAssetManifestTest, CanonicalDocumentValidatesAgainstCommittedDraft7Schema) {
    std::ifstream stream{CS_AVATAR_ASSET_SCHEMA_PATH, std::ios::binary};
    ASSERT_TRUE(stream);
    const json schema = json::parse(stream);
    nlohmann::json_schema::json_validator validator{
        nullptr, nlohmann::json_schema::default_string_format_check};
    ASSERT_NO_THROW(validator.set_root_schema(schema));
    EXPECT_NO_THROW(validator.validate(AvatarAssetManifestCodec{}.toJson(validManifest())));
}

TEST(AvatarAssetManifestTest, RejectsUnknownFieldsAndNativeWidthFutureVersions) {
    auto document = AvatarAssetManifestCodec{}.toJson(validManifest());
    document["surprise"] = true;
    EXPECT_EQ(AvatarAssetManifestCodec{}.fromJson(document).error().code(),
              ErrorCode::ParseFailure);
    document.erase("surprise");

    document["schemaVersion"] = static_cast<std::int64_t>(4294967297LL);
    EXPECT_EQ(AvatarAssetManifestCodec{}.fromJson(document).error().code(),
              ErrorCode::UnsupportedVersion);
    document["schemaVersion"] = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(AvatarAssetManifestCodec{}.fromJson(document).error().code(),
              ErrorCode::UnsupportedVersion);
}

TEST(AvatarAssetManifestTest, RejectsNonpositiveAndNonintegerVersions) {
    auto document = AvatarAssetManifestCodec{}.toJson(validManifest());
    for (const json version : {json{0}, json{-1}, json{1.0}}) {
        document["schemaVersion"] = version;
        EXPECT_EQ(AvatarAssetManifestCodec{}.fromJson(document).error().code(),
                  ErrorCode::ParseFailure);
    }
}

TEST(AvatarAssetManifestTest, FactoryRejectsDuplicateDeclarations) {
    auto draft = validDraft();
    draft.payloads.push_back(draft.payloads.front());
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.payloads[1].sha256 = draft.payloads[0].sha256;
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.allowedSlots.push_back(draft.allowedSlots.front());
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.supportedRigFamilies.push_back(draft.supportedRigFamilies.front());
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.supportedRepresentations.push_back(draft.supportedRepresentations.front());
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.grants.push_back(draft.grants.front());
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());
}

TEST(AvatarAssetManifestTest, FactoryRejectsInvalidNamesVersionsAndHashes) {
    auto draft = validDraft();
    draft.displayName = std::string{"\xC0\x80", 2};
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.assetVersion = "1.0";
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.payloads[0].sha256[0] = 'A';
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());
}

TEST(AvatarAssetManifestTest, FactoryRejectsMissingCompatibilityOrPayload) {
    auto draft = validDraft();
    draft.supportedRepresentations.clear();
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());
    draft = validDraft();
    draft.supportedRigFamilies.clear();
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());
    draft = validDraft();
    draft.allowedSlots.clear();
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());
    draft = validDraft();
    draft.payloads.clear();
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());
}

TEST(AvatarAssetManifestTest, FactoryRejectsEmptyAttributionAndInvalidValidityWindow) {
    auto draft = validDraft();
    draft.attributionText.clear();
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());
    draft = validDraft();
    draft.validUntil = draft.validFrom;
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());
}

TEST(AvatarAssetManifestTest, CoreAssetsRequireExplicitNonconditionalCommercialGrants) {
    auto draft = validDraft();
    draft.assetId = AvatarAssetId::create("core.body.base").value();
    draft.grants.erase(draft.grants.begin());
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.assetId = AvatarAssetId::create("core.body.base").value();
    draft.grants[0] = {AvatarRight::CommercialBroadcast, GrantState::Conditional,
                       "approval"};
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());
}

TEST(AvatarAssetManifestTest, SaveAndLoadUseCanonicalJson) {
    const auto path = std::filesystem::path{testing::TempDir()} /
                      "avatar-asset-manifest.json";
    ASSERT_TRUE(AvatarAssetManifestCodec{}.save(path, validManifest()).hasValue());
    const auto loaded = AvatarAssetManifestCodec{}.load(path);
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
    std::ifstream stream{path, std::ios::binary};
    const std::string persisted{std::istreambuf_iterator<char>{stream},
                                std::istreambuf_iterator<char>{}};
    EXPECT_EQ(persisted,
              AvatarAssetManifestCodec{}.toJson(loaded.value()).dump(2));
    stream.close();
    std::filesystem::remove(path);
}

TEST(AvatarAssetManifestTest, RejectsTraversalMalformedAndOversizedInput) {
    EXPECT_EQ(AvatarAssetManifestCodec{}
                  .load(std::filesystem::path{".."} / "manifest.json")
                  .error()
                  .code(),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(AvatarAssetManifestCodec{}
                  .save(std::filesystem::path{".."} / "manifest.json",
                        validManifest())
                  .error()
                  .code(),
              ErrorCode::InvalidArgument);

    const auto path = std::filesystem::path{testing::TempDir()} /
                      "avatar-asset-invalid.json";
    {
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        stream << "{ malformed";
    }
    EXPECT_EQ(AvatarAssetManifestCodec{}.load(path).error().code(),
              ErrorCode::ParseFailure);
    {
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        stream << std::string(8U * 1024U * 1024U + 1U, ' ');
    }
    EXPECT_EQ(AvatarAssetManifestCodec{}.load(path).error().code(),
              ErrorCode::ParseFailure);
    std::filesystem::remove(path);
}

}  // namespace
