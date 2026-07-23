#include "avatar/AvatarSpec.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace {

using creator::avatar::AssetRef;
using creator::avatar::AvatarAssetId;
using creator::avatar::AvatarId;
using creator::avatar::AvatarRepresentation;
using creator::avatar::AvatarSlot;
using creator::avatar::AvatarSpec;
using creator::avatar::AvatarSpecDraft;
using creator::avatar::ColorRgba;
using creator::avatar::MaterialOverride;
using creator::avatar::NamedScalar;
using creator::avatar::RigFamily;
using creator::core::AppError;
using creator::core::ErrorCode;

void expectMetadata(const AppError& error, std::string_view issueCode,
                    std::string_view messageKey) {
    ASSERT_TRUE(error.issueCode().has_value());
    EXPECT_EQ(*error.issueCode(), issueCode);
    ASSERT_TRUE(error.messageKey().has_value());
    EXPECT_EQ(*error.messageKey(), messageKey);
}

AvatarSpecDraft validDraft() {
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
            {AvatarSlot::Body,
             {AvatarAssetId::create("core.body.base").value(), "1.0.0", "default"}},
            {AvatarSlot::Head,
             {AvatarAssetId::create("core.head.base").value(), "1.0.0", "default"}},
            {AvatarSlot::Eyes,
             {AvatarAssetId::create("core.eyes.round").value(), "1.0.0", "brown"}},
            {AvatarSlot::Mouth,
             {AvatarAssetId::create("core.mouth.smile").value(), "1.0.0", "default"}},
            {AvatarSlot::HairFront,
             {AvatarAssetId::create("core.hair.front.soft-bob").value(), "1.0.0", "default"}},
        },
        .palette = {{"skin", {0.9F, 0.7F, 0.6F, 1.0F}}},
        .materials = {{"fabric", {0.3F, 0.2F, 0.1F, 1.0F}, 0.0F, 0.8F, 0.0F, 1.0F}},
        .expressions = {{"happy", 0.5F}},
        .physics = {{"hair-sway", 0.1F}},
        .trackingProfileId = "arkit-basic",
    };
    return draft;
}

TEST(AvatarSpecTest, AcceptsDeterministicallyOrderedSlotsAndMorphs) {
    const auto spec = AvatarSpec::create(validDraft());
    ASSERT_TRUE(spec.hasValue()) << spec.error().message();
    EXPECT_EQ(spec.value().schemaVersion(), 1);
    EXPECT_EQ(spec.value().rigFamily(), RigFamily::Humanoid);
    EXPECT_EQ(spec.value().slots().at(AvatarSlot::HairFront).assetId.value(),
              "core.hair.front.soft-bob");
}

TEST(AvatarSpecTest, RejectsNonFiniteOrOutOfRangeMorphs) {
    auto draft = validDraft();
    draft.bodyMorphs = {{"height", 1.01F}};
    auto result = AvatarSpec::create(std::move(draft));
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    expectMetadata(result.error(), "avatar.spec.named-scalar", "avatar.validation.named-scalar");

    draft = validDraft();
    draft.faceMorphs = {{"eye-width", std::numeric_limits<float>::quiet_NaN()}};
    result = AvatarSpec::create(std::move(draft));
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    expectMetadata(result.error(), "avatar.spec.named-scalar", "avatar.validation.named-scalar");
}

TEST(AvatarSpecTest, RejectsDuplicateNamedValuesAndIncompatibleRepresentation) {
    auto draft = validDraft();
    draft.faceMorphs = {{"eye-width", 0.2F}, {"eye-width", 0.3F}};
    auto result = AvatarSpec::create(std::move(draft));
    EXPECT_FALSE(result.hasValue());
    expectMetadata(result.error(), "avatar.spec.named-scalar", "avatar.validation.named-scalar");

    draft = validDraft();
    draft.rigFamily = RigFamily::Quadruped;
    draft.preferredRepresentation = AvatarRepresentation::Vrm1;
    result = AvatarSpec::create(std::move(draft));
    EXPECT_FALSE(result.hasValue());
    expectMetadata(result.error(), "avatar.spec.representation", "avatar.validation.representation");
}

TEST(AvatarSpecTest, RejectsInvalidTextSemanticVersionsAndMissingRequiredSlots) {
    auto draft = validDraft();
    draft.displayName = "\xFF";
    auto result = AvatarSpec::create(std::move(draft));
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    expectMetadata(result.error(), "avatar.spec.display-name", "avatar.validation.display-name");

    draft = validDraft();
    draft.slots.at(AvatarSlot::Body).version = "1.0";
    result = AvatarSpec::create(std::move(draft));
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    expectMetadata(result.error(), "avatar.spec.asset-reference",
                   "avatar.validation.asset-reference");

    draft = validDraft();
    draft.slots.erase(AvatarSlot::Mouth);
    result = AvatarSpec::create(std::move(draft));
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    expectMetadata(result.error(), "avatar.spec.required-slots",
                   "avatar.validation.required-slots");
}

TEST(AvatarSpecTest, RejectsInvalidColorsAndMaterialValues) {
    auto draft = validDraft();
    draft.palette.at("skin").alpha = 1.1F;
    auto result = AvatarSpec::create(std::move(draft));
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    expectMetadata(result.error(), "avatar.spec.palette", "avatar.validation.palette");

    draft = validDraft();
    draft.materials.front().roughness = std::numeric_limits<float>::infinity();
    result = AvatarSpec::create(std::move(draft));
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    expectMetadata(result.error(), "avatar.spec.material", "avatar.validation.material");
}

TEST(AvatarSpecTest, RequiresNamedFamilyThemeAndTrackingFieldsWithinTwoHundredCodePoints) {
    auto draft = validDraft();
    draft.speciesFamily.clear();
    auto result = AvatarSpec::create(std::move(draft));
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    expectMetadata(result.error(), "avatar.spec.named-field", "avatar.validation.named-field");

    draft = validDraft();
    draft.styleTheme = "\xFF";
    result = AvatarSpec::create(std::move(draft));
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    expectMetadata(result.error(), "avatar.spec.named-field", "avatar.validation.named-field");

    draft = validDraft();
    draft.trackingProfileId = std::string(201, 'x');
    result = AvatarSpec::create(std::move(draft));
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    expectMetadata(result.error(), "avatar.spec.named-field", "avatar.validation.named-field");

    draft = validDraft();
    draft.speciesFamily = std::string(200, 's');
    draft.styleTheme = std::string(200, 't');
    draft.trackingProfileId = std::string(200, 'p');
    EXPECT_TRUE(AvatarSpec::create(std::move(draft)).hasValue());
}

TEST(AvatarSpecTest, RequiresGltfRigForNonHumanoidAnimalFamilies) {
    auto draft = validDraft();
    draft.rigFamily = RigFamily::Avian;
    EXPECT_EQ(AvatarSpec::create(std::move(draft)).error().code(),
              ErrorCode::InvalidArgument);

    draft = validDraft();
    draft.rigFamily = RigFamily::Avian;
    draft.preferredRepresentation = AvatarRepresentation::GltfRig;
    EXPECT_TRUE(AvatarSpec::create(std::move(draft)).hasValue());
}

}  // namespace
