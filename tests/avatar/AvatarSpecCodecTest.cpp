#include "avatar/AvatarSpecCodec.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

namespace {

using creator::avatar::AssetRef;
using creator::avatar::AvatarAssetId;
using creator::avatar::AvatarId;
using creator::avatar::AvatarRepresentation;
using creator::avatar::AvatarSlot;
using creator::avatar::AvatarSpec;
using creator::avatar::AvatarSpecCodec;
using creator::avatar::AvatarSpecDraft;
using creator::avatar::ColorRgba;
using creator::avatar::MaterialOverride;
using creator::avatar::NamedScalar;
using creator::avatar::RigFamily;
using creator::core::ErrorCode;

AvatarSpec validSpec() {
    AvatarSpecDraft draft{
        .avatarId = AvatarId::create("avatar.soft-bob").value(),
        .displayName = "Soft Bob",
        .rigFamily = RigFamily::Humanoid,
        .speciesFamily = "human",
        .styleTheme = "casual",
        .preferredRepresentation = AvatarRepresentation::Inochi2d,
        .bodyMorphs = {{"height", 0.2F}},
        .faceMorphs = {{"eye-width", -0.1F}},
        .animalMorphs = {},
        .slots = {
            {AvatarSlot::Body, {AvatarAssetId::create("core.body.base").value(), "1.0.0", "default"}},
            {AvatarSlot::Head, {AvatarAssetId::create("core.head.base").value(), "1.0.0", "default"}},
            {AvatarSlot::Eyes, {AvatarAssetId::create("core.eyes.round").value(), "1.0.0", "brown"}},
            {AvatarSlot::Mouth, {AvatarAssetId::create("core.mouth.smile").value(), "1.0.0", "default"}},
        },
        .palette = {{"skin", {0.9F, 0.7F, 0.6F, 1.0F}}},
        .materials = {{"fabric", {0.3F, 0.2F, 0.1F, 1.0F}, 0.0F, 0.8F, 0.0F, 1.0F}},
        .expressions = {{"happy", 0.5F}},
        .physics = {{"hair-sway", 0.1F}},
        .trackingProfileId = "arkit-basic",
    };
    return AvatarSpec::create(std::move(draft)).value();
}

TEST(AvatarSpecCodecTest, RoundTripIsCanonicalAndStable) {
    const auto first = AvatarSpecCodec{}.toJson(validSpec());
    const auto decoded = AvatarSpecCodec{}.fromJson(first);
    ASSERT_TRUE(decoded.hasValue()) << decoded.error().message();
    EXPECT_EQ(AvatarSpecCodec{}.toJson(decoded.value()), first);
}

TEST(AvatarSpecCodecTest, RejectsUnknownFieldAndFutureVersion) {
    auto json = AvatarSpecCodec{}.toJson(validSpec());
    json["surprise"] = true;
    EXPECT_EQ(AvatarSpecCodec{}.fromJson(json).error().code(), ErrorCode::ParseFailure);
    json.erase("surprise");
    json["schemaVersion"] = AvatarSpec::kCurrentSchemaVersion + 1;
    EXPECT_EQ(AvatarSpecCodec{}.fromJson(json).error().code(), ErrorCode::UnsupportedVersion);
}

TEST(AvatarSpecCodecTest, RejectsTraversalPathOnLoad) {
    const auto result = AvatarSpecCodec{}.load(std::filesystem::path{".."} / "avatar.json");
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(AvatarSpecCodecTest, RejectsMalformedInputAndMissingFile) {
    const auto path = std::filesystem::path{testing::TempDir()} / "avatar-spec-codec-malformed.json";
    std::filesystem::remove(path);
    EXPECT_EQ(AvatarSpecCodec{}.load(path).error().code(), ErrorCode::NotFound);

    {
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        stream << "{ malformed";
    }
    EXPECT_EQ(AvatarSpecCodec{}.load(path).error().code(), ErrorCode::ParseFailure);
    std::filesystem::remove(path);
}

TEST(AvatarSpecCodecTest, RejectsInputLargerThanEightMebibytes) {
    const auto path = std::filesystem::path{testing::TempDir()} / "avatar-spec-codec-oversized.json";
    {
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        stream << std::string(8U * 1024U * 1024U + 1U, ' ');
    }
    EXPECT_EQ(AvatarSpecCodec{}.load(path).error().code(), ErrorCode::ParseFailure);
    std::filesystem::remove(path);
}

TEST(AvatarSpecCodecTest, CanonicalizesNamedAndMaterialOrderWhenSaving) {
    auto document = AvatarSpecCodec{}.toJson(validSpec());
    document["bodyMorphs"] = {{{"name", "zeta"}, {"value", 0.1F}},
                              {{"name", "alpha"}, {"value", -0.1F}}};
    document["materials"] = {
        {{"channel", "zeta"}, {"baseColor", {{"red", 0.0F}, {"green", 0.0F}, {"blue", 0.0F}, {"alpha", 1.0F}}},
         {"metallic", 0.0F}, {"roughness", 1.0F}, {"emission", 0.0F}, {"opacity", 1.0F}},
        {{"channel", "alpha"}, {"baseColor", {{"red", 0.0F}, {"green", 0.0F}, {"blue", 0.0F}, {"alpha", 1.0F}}},
         {"metallic", 0.0F}, {"roughness", 1.0F}, {"emission", 0.0F}, {"opacity", 1.0F}},
    };
    const auto decoded = AvatarSpecCodec{}.fromJson(document);
    ASSERT_TRUE(decoded.hasValue()) << decoded.error().message();
    const auto canonical = AvatarSpecCodec{}.toJson(decoded.value());
    EXPECT_EQ(canonical["bodyMorphs"][0]["name"], "alpha");
    EXPECT_EQ(canonical["materials"][0]["channel"], "alpha");

    const auto path = std::filesystem::path{testing::TempDir()} / "avatar-spec-codec-canonical.json";
    ASSERT_TRUE(AvatarSpecCodec{}.save(path, decoded.value()).hasValue());
    std::ifstream stream{path, std::ios::binary};
    const std::string persisted{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    EXPECT_EQ(persisted, canonical.dump(2));
    stream.close();
    std::filesystem::remove(path);
}

}  // namespace
