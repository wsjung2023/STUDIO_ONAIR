#include "avatar/AvatarSpec.h"

#include <gtest/gtest.h>

#include <limits>
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
using creator::core::ErrorCode;

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
    EXPECT_EQ(AvatarSpec::create(std::move(draft)).error().code(),
              ErrorCode::InvalidArgument);

    draft = validDraft();
    draft.faceMorphs = {{"eye-width", std::numeric_limits<float>::quiet_NaN()}};
    EXPECT_EQ(AvatarSpec::create(std::move(draft)).error().code(),
              ErrorCode::InvalidArgument);
}

TEST(AvatarSpecTest, RejectsDuplicateNamedValuesAndIncompatibleRepresentation) {
    auto draft = validDraft();
    draft.faceMorphs = {{"eye-width", 0.2F}, {"eye-width", 0.3F}};
    EXPECT_FALSE(AvatarSpec::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.rigFamily = RigFamily::Quadruped;
    draft.preferredRepresentation = AvatarRepresentation::Vrm1;
    EXPECT_FALSE(AvatarSpec::create(std::move(draft)).hasValue());
}

TEST(AvatarSpecTest, RejectsInvalidTextSemanticVersionsAndMissingRequiredSlots) {
    auto draft = validDraft();
    draft.displayName = "\xFF";
    EXPECT_EQ(AvatarSpec::create(std::move(draft)).error().code(),
              ErrorCode::InvalidArgument);

    draft = validDraft();
    draft.slots.at(AvatarSlot::Body).version = "1.0";
    EXPECT_EQ(AvatarSpec::create(std::move(draft)).error().code(),
              ErrorCode::InvalidArgument);

    draft = validDraft();
    draft.slots.erase(AvatarSlot::Mouth);
    EXPECT_EQ(AvatarSpec::create(std::move(draft)).error().code(),
              ErrorCode::InvalidArgument);
}

TEST(AvatarSpecTest, RejectsInvalidColorsAndMaterialValues) {
    auto draft = validDraft();
    draft.palette.at("skin").alpha = 1.1F;
    EXPECT_EQ(AvatarSpec::create(std::move(draft)).error().code(),
              ErrorCode::InvalidArgument);

    draft = validDraft();
    draft.materials.front().roughness = std::numeric_limits<float>::infinity();
    EXPECT_EQ(AvatarSpec::create(std::move(draft)).error().code(),
              ErrorCode::InvalidArgument);
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
