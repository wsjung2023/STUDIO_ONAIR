#include "avatar/AvatarSoftwareRasterizer.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using creator::avatar::AvatarMeshVertex;
using creator::avatar::AvatarSoftwareRasterizer;
using creator::avatar::AvatarTexture;
using creator::core::ErrorCode;
using creator::core::TimestampNs;

TEST(AvatarSoftwareRasterizerTest, RasterizesTexturedTriangleIntoBgraFrame) {
    const AvatarTexture texture{1, 1, {12, 34, 56, 255}};
    const AvatarMeshVertex vertices[] = {
        {0.0F, 0.0F, 0.0F, 0.0F},
        {4.0F, 0.0F, 1.0F, 0.0F},
        {0.0F, 4.0F, 0.0F, 1.0F},
    };
    const std::uint32_t indices[] = {0, 1, 2};
    const auto result = AvatarSoftwareRasterizer::render(
        TimestampNs{}, 4, 4, vertices, indices, texture);
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_EQ(result.value().timestamp(), TimestampNs{});
    EXPECT_EQ(result.value().width(), 4U);
    EXPECT_EQ(result.value().bytes()[4U * 4U + 4U], 12U);
    EXPECT_EQ(result.value().bytes()[4U * 4U + 5U], 34U);
    EXPECT_EQ(result.value().bytes()[4U * 4U + 6U], 56U);
    EXPECT_EQ(result.value().bytes()[4U * 4U + 7U], 255U);
}

TEST(AvatarSoftwareRasterizerTest, RejectsMalformedMeshAndTexture) {
    const AvatarTexture texture{1, 1, {0, 0, 0}};
    const AvatarMeshVertex vertices[] = {{0.0F, 0.0F, 0.0F, 0.0F}};
    const std::uint32_t indices[] = {0, 1};
    const auto result = AvatarSoftwareRasterizer::render(
        TimestampNs{}, 4, 4, vertices, indices, texture);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

}  // namespace
