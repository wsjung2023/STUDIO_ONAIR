#include "avatar/CharacterAvatarRenderer.h"

#include "avatar/AvatarParameterMapper.h"
#include "avatar/ExpressionParameters.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using creator::avatar::AvatarCharacterId;
using creator::avatar::AvatarCorner;
using creator::avatar::AvatarParameterMapper;
using creator::avatar::AvatarPlacement;
using creator::avatar::AvatarPlacementMode;
using creator::avatar::CharacterAvatarRenderer;
using creator::avatar::ExpressionParameters;
using creator::avatar::characterAvatarBindings;
using creator::core::ErrorCode;
using creator::core::TimestampNs;

AvatarParameterMapper mapper() {
    return AvatarParameterMapper::create(characterAvatarBindings()).value();
}

std::vector<std::uint8_t> renderBytes(CharacterAvatarRenderer& renderer,
                                      const AvatarParameterMapper& map,
                                      const ExpressionParameters& expression) {
    const auto mapped = map.map(expression);
    EXPECT_TRUE(mapped.hasValue());
    const auto frame = renderer.render(TimestampNs{}, mapped.value());
    EXPECT_TRUE(frame.hasValue());
    const auto bytes = frame.value().bytes();
    return std::vector<std::uint8_t>{bytes.begin(), bytes.end()};
}

TEST(CharacterAvatarRendererTest, CatalogHasThreeDistinctCharacters) {
    const auto catalog = creator::avatar::avatarCharacterCatalog();
    ASSERT_EQ(catalog.size(), 3U);
    EXPECT_EQ(catalog[0].id, AvatarCharacterId::Human);
    EXPECT_EQ(catalog[1].id, AvatarCharacterId::Cat);
    EXPECT_EQ(catalog[2].id, AvatarCharacterId::Fox);
    EXPECT_FALSE(catalog[0].labelKo.empty());
}

TEST(CharacterAvatarRendererTest, BindingsCoverAllNineChannels) {
    EXPECT_EQ(characterAvatarBindings().size(), 9U);
    EXPECT_TRUE(AvatarParameterMapper::create(characterAvatarBindings()).hasValue());
}

TEST(CharacterAvatarRendererTest, RejectsEmptyDimensions) {
    CharacterAvatarRenderer renderer{0, 0};
    const auto result = renderer.render(TimestampNs{}, {});
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(CharacterAvatarRendererTest, FrontModeIsOpaqueAndNonFlat) {
    CharacterAvatarRenderer renderer{160, 160, AvatarCharacterId::Human,
                                     {AvatarPlacementMode::Front, {}}, 1};
    const auto bytes = renderBytes(renderer, mapper(), ExpressionParameters::neutral());
    ASSERT_EQ(bytes.size(), 160U * 160U * 4U);
    bool nonFlat = false;
    bool anyOpaque = false;
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        if (i >= 4 && (bytes[i] != bytes[0] || bytes[i + 1] != bytes[1] ||
                       bytes[i + 2] != bytes[2])) {
            nonFlat = true;
        }
        if (bytes[i + 3] == 255) anyOpaque = true;
    }
    EXPECT_TRUE(nonFlat);
    EXPECT_TRUE(anyOpaque);  // front mode fills an opaque backdrop
}

TEST(CharacterAvatarRendererTest, CornerModeLeavesTransparentBackground) {
    CharacterAvatarRenderer renderer{
        200, 200, AvatarCharacterId::Cat,
        {AvatarPlacementMode::Corner, AvatarCorner::RightBottom}, 1};
    const auto bytes = renderBytes(renderer, mapper(), ExpressionParameters::neutral());
    // The top-left corner is far from a bottom-right avatar: fully transparent,
    // so the frame composites over a screen recording.
    EXPECT_EQ(bytes[3], 0);
    // But some pixels are drawn (the avatar + its card), so it is not empty.
    bool anyDrawn = false;
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        if (bytes[i + 3] > 0) {
            anyDrawn = true;
            break;
        }
    }
    EXPECT_TRUE(anyDrawn);
}

TEST(CharacterAvatarRendererTest, ExpressionAndCharacterChangeThePixels) {
    CharacterAvatarRenderer renderer{160, 160, AvatarCharacterId::Human,
                                     {AvatarPlacementMode::Front, {}}, 1};
    const auto map = mapper();

    ExpressionParameters open = ExpressionParameters::neutral();
    open.eyeOpenLeft = 1.0F;
    open.eyeOpenRight = 1.0F;
    ExpressionParameters blink = open;
    blink.eyeOpenLeft = 0.02F;
    blink.eyeOpenRight = 0.02F;
    blink.mouthOpen = 1.0F;

    const auto a = renderBytes(renderer, map, open);
    const auto b = renderBytes(renderer, map, blink);
    EXPECT_NE(a, b);  // blink + mouth moved -> pixels changed

    // Swapping the character live changes the rendering too.
    renderer.setCharacter(AvatarCharacterId::Fox);
    const auto c = renderBytes(renderer, map, open);
    EXPECT_NE(a, c);
    EXPECT_EQ(renderer.character(), AvatarCharacterId::Fox);
}

}  // namespace
