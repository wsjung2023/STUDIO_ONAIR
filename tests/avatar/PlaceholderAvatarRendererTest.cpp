#include "avatar/PlaceholderAvatarRenderer.h"

#include "avatar/AvatarParameterMapper.h"
#include "avatar/ExpressionParameters.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using creator::avatar::AvatarParameterMapper;
using creator::avatar::ExpressionParameters;
using creator::avatar::PlaceholderAvatarRenderer;
using creator::avatar::placeholderAvatarBindings;
using creator::core::ErrorCode;
using creator::core::TimestampNs;

AvatarParameterMapper mapper() {
    return AvatarParameterMapper::create(placeholderAvatarBindings()).value();
}

std::vector<std::uint8_t> renderBytes(PlaceholderAvatarRenderer& renderer,
                                      const AvatarParameterMapper& map,
                                      const ExpressionParameters& expression) {
    const auto mapped = map.map(expression);
    EXPECT_TRUE(mapped.hasValue());
    const auto frame = renderer.render(TimestampNs{}, mapped.value());
    EXPECT_TRUE(frame.hasValue());
    const auto bytes = frame.value().bytes();
    return std::vector<std::uint8_t>{bytes.begin(), bytes.end()};
}

TEST(PlaceholderAvatarRendererTest, BindingsCoverAllNineChannels) {
    EXPECT_EQ(placeholderAvatarBindings().size(), 9U);
    EXPECT_TRUE(AvatarParameterMapper::create(placeholderAvatarBindings())
                    .hasValue());
}

TEST(PlaceholderAvatarRendererTest, RejectsEmptyDimensions) {
    PlaceholderAvatarRenderer renderer{0, 0};
    const auto result = renderer.render(TimestampNs{}, {});
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(PlaceholderAvatarRendererTest, ProducesANonFlatFrame) {
    PlaceholderAvatarRenderer renderer{160, 120};
    const auto bytes = renderBytes(renderer, mapper(), ExpressionParameters::neutral());
    ASSERT_EQ(bytes.size(), 160U * 120U * 4U);
    // A rendered face is not a flat fill: at least one pixel differs from the
    // first, so the avatar track carries real content.
    bool nonFlat = false;
    for (std::size_t i = 4; i < bytes.size(); i += 4) {
        if (bytes[i] != bytes[0] || bytes[i + 1] != bytes[1] ||
            bytes[i + 2] != bytes[2]) {
            nonFlat = true;
            break;
        }
    }
    EXPECT_TRUE(nonFlat);
}

TEST(PlaceholderAvatarRendererTest, ExpressionParametersChangeThePixels) {
    PlaceholderAvatarRenderer renderer{160, 120};
    const auto map = mapper();

    ExpressionParameters closed = ExpressionParameters::neutral();
    closed.eyeOpenLeft = 1.0F;
    closed.eyeOpenRight = 1.0F;
    closed.mouthOpen = 0.0F;

    ExpressionParameters open = closed;
    open.mouthOpen = 1.0F;
    open.eyeOpenLeft = 0.05F;
    open.eyeOpenRight = 0.05F;

    const auto a = renderBytes(renderer, map, closed);
    const auto b = renderBytes(renderer, map, open);
    ASSERT_EQ(a.size(), b.size());
    EXPECT_NE(a, b);  // eyes/mouth moved -> pixels changed
}

}  // namespace
