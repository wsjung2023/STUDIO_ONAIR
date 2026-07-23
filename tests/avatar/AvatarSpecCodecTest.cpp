#include "avatar/AvatarSpecCodec.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

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

std::filesystem::path temporaryPath(std::string_view name) {
    return std::filesystem::path{testing::TempDir()} / name;
}

std::size_t temporarySiblingCount(const std::filesystem::path& target) {
    std::size_t count = 0;
    const std::string prefix = "." + target.filename().string() + ".part-";
    for (const auto& entry : std::filesystem::directory_iterator{target.parent_path()}) {
        if (entry.path().filename().string().starts_with(prefix)) ++count;
    }
    return count;
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

TEST(AvatarSpecCodecTest, RejectsArbitraryIntegerFutureVersionsWithoutNarrowing) {
    auto document = AvatarSpecCodec{}.toJson(validSpec());
    document["schemaVersion"] = static_cast<std::int64_t>(4294967297LL);
    EXPECT_EQ(AvatarSpecCodec{}.fromJson(document).error().code(), ErrorCode::UnsupportedVersion);

    document["schemaVersion"] = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(AvatarSpecCodec{}.fromJson(document).error().code(), ErrorCode::UnsupportedVersion);
}

TEST(AvatarSpecCodecTest, RejectsNonpositiveAndNonintegerSchemaVersionsAsParseFailures) {
    auto document = AvatarSpecCodec{}.toJson(validSpec());
    document["schemaVersion"] = 0;
    EXPECT_EQ(AvatarSpecCodec{}.fromJson(document).error().code(), ErrorCode::ParseFailure);

    document["schemaVersion"] = -1;
    EXPECT_EQ(AvatarSpecCodec{}.fromJson(document).error().code(), ErrorCode::ParseFailure);

    document["schemaVersion"] = 1.0;
    EXPECT_EQ(AvatarSpecCodec{}.fromJson(document).error().code(), ErrorCode::ParseFailure);
}

TEST(AvatarSpecCodecTest, RejectsTraversalPathOnLoad) {
    const auto result = AvatarSpecCodec{}.load(std::filesystem::path{".."} / "avatar.json");
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(AvatarSpecCodecTest, RejectsTraversalPathOnSave) {
    const auto result = AvatarSpecCodec{}.save(std::filesystem::path{".."} / "avatar.json", validSpec());
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

TEST(AvatarSpecCodecTest, SavesAndLoadsTheSameCanonicalSpec) {
    const auto path = temporaryPath("avatar-spec-codec-save-load.json");
    std::filesystem::remove(path);
    ASSERT_TRUE(AvatarSpecCodec{}.save(path, validSpec()).hasValue());
    const auto loaded = AvatarSpecCodec{}.load(path);
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
    EXPECT_EQ(AvatarSpecCodec{}.toJson(loaded.value()), AvatarSpecCodec{}.toJson(validSpec()));
    std::filesystem::remove(path);
}

TEST(AvatarSpecCodecTest, ReturnsTypedErrorForInvalidUtf8WithoutLeavingTemporaryFile) {
    auto draft = validSpec().values();
    draft.avatarId = AvatarId::create("\xFF").value();
    const auto invalidUtf8Spec = AvatarSpec::create(std::move(draft));
    ASSERT_TRUE(invalidUtf8Spec.hasValue());
    const auto path = temporaryPath("avatar-spec-codec-invalid-utf8.json");
    std::filesystem::remove(path);
    EXPECT_EQ(temporarySiblingCount(path), 0U);

    creator::core::Result<void> result;
    EXPECT_NO_THROW(result = AvatarSpecCodec{}.save(path, invalidUtf8Spec.value()));
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::ParseFailure);
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_EQ(temporarySiblingCount(path), 0U);
}

TEST(AvatarSpecCodecTest, LeavesDestinationAndNoTemporaryFileWhenPublishFails) {
#ifdef _WIN32
    const auto path = temporaryPath("avatar-spec-codec-publish-failure.json");
    {
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        stream << "last-good";
    }
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    const auto result = AvatarSpecCodec{}.save(path, validSpec());
    EXPECT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::IoFailure);
    CloseHandle(handle);

    std::ifstream stream{path, std::ios::binary};
    const std::string contents{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    EXPECT_EQ(contents, "last-good");
    stream.close();
    EXPECT_EQ(temporarySiblingCount(path), 0U);
    std::filesystem::remove(path);
#else
    GTEST_SKIP() << "exclusive replacement failure is a Windows filesystem behavior";
#endif
}

}  // namespace
