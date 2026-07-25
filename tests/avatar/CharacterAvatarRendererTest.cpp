#include "avatar/CharacterAvatarRenderer.h"

#include "avatar/AvatarParameterMapper.h"
#include "avatar/ExpressionParameters.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
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

TEST(CharacterAvatarRendererTest, CatalogHasElevenDistinctCharacters) {
    const auto catalog = creator::avatar::avatarCharacterCatalog();
    ASSERT_EQ(catalog.size(), 11U);
    EXPECT_EQ(catalog[0].id, AvatarCharacterId::Human);
    EXPECT_EQ(catalog[1].id, AvatarCharacterId::Cat);
    EXPECT_EQ(catalog[2].id, AvatarCharacterId::Fox);
    // Every entry has a machine key and a non-empty Korean label, and both the
    // ids and the keys are unique.
    std::vector<std::string> keys;
    std::vector<AvatarCharacterId> ids;
    for (const auto& info : catalog) {
        EXPECT_FALSE(info.key.empty());
        EXPECT_FALSE(info.labelKo.empty());
        keys.push_back(info.key);
        ids.push_back(info.id);
    }
    std::sort(keys.begin(), keys.end());
    EXPECT_EQ(std::adjacent_find(keys.begin(), keys.end()), keys.end());
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(std::adjacent_find(ids.begin(), ids.end()), ids.end());
}

// Every catalog character renders a full, correctly sized frame in both an
// opaque Front and a transparent overlay placement, with alpha actually present.
TEST(CharacterAvatarRendererTest, EveryCharacterRendersWithAlpha) {
    const auto map = mapper();
    for (const auto& info : creator::avatar::avatarCharacterCatalog()) {
        CharacterAvatarRenderer front{140, 140, info.id,
                                      {AvatarPlacementMode::Front, {}}, 1};
        const auto fb = renderBytes(front, map, ExpressionParameters::neutral());
        ASSERT_EQ(fb.size(), 140U * 140U * 4U) << info.key;
        bool anyOpaque = false;
        bool anyColour = false;
        for (std::size_t i = 0; i < fb.size(); i += 4) {
            if (fb[i + 3] == 255) anyOpaque = true;
            if (fb[i] != fb[0] || fb[i + 1] != fb[1] || fb[i + 2] != fb[2])
                anyColour = true;
        }
        EXPECT_TRUE(anyOpaque) << info.key;   // opaque front backdrop
        EXPECT_TRUE(anyColour) << info.key;   // the character is actually drawn

        CharacterAvatarRenderer overlay{
            160, 160, info.id,
            {AvatarPlacementMode::Corner, AvatarCorner::RightBottom}, 1};
        const auto ob = renderBytes(overlay, map, ExpressionParameters::neutral());
        bool anyTransparent = false;
        bool anyDrawn = false;
        for (std::size_t i = 0; i < ob.size(); i += 4) {
            if (ob[i + 3] == 0) anyTransparent = true;
            if (ob[i + 3] > 0) anyDrawn = true;
        }
        EXPECT_TRUE(anyTransparent) << info.key;  // composites over a screen
        EXPECT_TRUE(anyDrawn) << info.key;
    }
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

namespace {

// Coverage (number of non-transparent pixels) of an overlay-mode render.
std::size_t coverage(const std::vector<std::uint8_t>& bytes) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < bytes.size(); i += 4)
        if (bytes[i + 3] > 0) ++n;
    return n;
}

// Horizontal centroid (in pixels) of the non-transparent content.
double centroidX(const std::vector<std::uint8_t>& bytes, std::uint32_t w,
                 std::uint32_t h) {
    double sum = 0.0;
    std::size_t n = 0;
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4U;
            if (bytes[i + 3] > 0) {
                sum += x;
                ++n;
            }
        }
    }
    return n == 0 ? 0.0 : sum / static_cast<double>(n);
}

}  // namespace

TEST(CharacterAvatarRendererTest, UserScaleResizesTheDrawnContent) {
    CharacterAvatarRenderer renderer{
        200, 200, AvatarCharacterId::Human,
        {AvatarPlacementMode::Corner, AvatarCorner::RightBottom}, 1};
    const auto map = mapper();

    renderer.setPosition(0.5F, 0.5F);
    renderer.setUserScale(0.5F);
    const auto small = renderBytes(renderer, map, ExpressionParameters::neutral());
    renderer.setUserScale(2.0F);
    const auto large = renderBytes(renderer, map, ExpressionParameters::neutral());

    EXPECT_GT(coverage(large), coverage(small) * 2U);  // clearly bigger
    EXPECT_FLOAT_EQ(renderer.userScale(), 2.0F);
}

TEST(CharacterAvatarRendererTest, UserScaleIsClampedToRange) {
    CharacterAvatarRenderer renderer{64, 64, AvatarCharacterId::Cat};
    renderer.setUserScale(100.0F);
    EXPECT_FLOAT_EQ(renderer.userScale(), CharacterAvatarRenderer::kMaxScale);
    renderer.setUserScale(0.0F);
    EXPECT_FLOAT_EQ(renderer.userScale(), CharacterAvatarRenderer::kMinScale);
}

TEST(CharacterAvatarRendererTest, PositionMovesTheDrawnContent) {
    constexpr std::uint32_t kW = 240, kH = 160;
    CharacterAvatarRenderer renderer{
        kW, kH, AvatarCharacterId::Fox,
        {AvatarPlacementMode::Corner, AvatarCorner::RightBottom}, 1};
    const auto map = mapper();
    renderer.setUserScale(0.8F);

    renderer.setPosition(0.25F, 0.5F);
    const auto leftBytes = renderBytes(renderer, map, ExpressionParameters::neutral());
    renderer.setPosition(0.75F, 0.5F);
    const auto rightBytes = renderBytes(renderer, map, ExpressionParameters::neutral());

    const double leftC = centroidX(leftBytes, kW, kH);
    const double rightC = centroidX(rightBytes, kW, kH);
    EXPECT_GT(rightC, leftC + static_cast<double>(kW) * 0.2);
    EXPECT_FLOAT_EQ(renderer.posX(), 0.75F);
}

TEST(CharacterAvatarRendererTest, PositionIsClampedSoAvatarIsNeverFullyLost) {
    constexpr std::uint32_t kW = 160, kH = 160;
    CharacterAvatarRenderer renderer{
        kW, kH, AvatarCharacterId::Bear,
        {AvatarPlacementMode::Corner, AvatarCorner::RightBottom}, 1};
    const auto map = mapper();
    // Push far off-frame; the setter clamps the normalised value and the render
    // clamps the head centre, so content still appears.
    renderer.setPosition(5.0F, -5.0F);
    EXPECT_FLOAT_EQ(renderer.posX(), 1.0F);
    EXPECT_FLOAT_EQ(renderer.posY(), 0.0F);
    const auto bytes = renderBytes(renderer, map, ExpressionParameters::neutral());
    EXPECT_GT(coverage(bytes), 0U);
}

TEST(CharacterAvatarRendererTest, PresetsSetTransformAndBackdropStyle) {
    CharacterAvatarRenderer renderer{160, 160, AvatarCharacterId::Girl};
    renderer.applyFrontPreset();
    EXPECT_FLOAT_EQ(renderer.posX(), 0.5F);
    EXPECT_FLOAT_EQ(renderer.userScale(), 1.0F);

    renderer.applyCornerPreset(AvatarCorner::LeftTop);
    EXPECT_LT(renderer.posX(), 0.5F);
    EXPECT_LT(renderer.posY(), 0.5F);
    EXPECT_LT(renderer.userScale(), 1.0F);
}

}  // namespace
