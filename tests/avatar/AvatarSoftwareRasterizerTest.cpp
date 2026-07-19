#include "avatar/AvatarSoftwareRasterizer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <iterator>
#include <vector>

namespace {

using creator::avatar::AvatarMeshVertex;
using creator::avatar::AvatarSoftwareRasterizer;
using creator::avatar::AvatarTexture;
using creator::avatar::AvatarSoftwareRenderInput;
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

TEST(AvatarSoftwareRasterizerTest, CompositesMultipleTexturedBatchesInOrder) {
    const AvatarMeshVertex vertices[] = {
        {0.0F, 0.0F, 0.0F, 0.0F},
        {4.0F, 0.0F, 1.0F, 0.0F},
        {0.0F, 4.0F, 0.0F, 1.0F},
    };
    const std::uint32_t indices[] = {0, 1, 2};
    const std::vector<AvatarSoftwareRenderInput> batches{
        {std::vector<AvatarMeshVertex>{std::begin(vertices), std::end(vertices)},
         std::vector<std::uint32_t>{std::begin(indices), std::end(indices)},
         AvatarTexture{1, 1, {0, 0, 255, 255}}},
        {std::vector<AvatarMeshVertex>{std::begin(vertices), std::end(vertices)},
         std::vector<std::uint32_t>{std::begin(indices), std::end(indices)},
         AvatarTexture{1, 1, {0, 255, 0, 128}}},
    };
    const auto result = AvatarSoftwareRasterizer::renderBatches(
        TimestampNs{}, 4, 4, batches);
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_EQ(result.value().bytes()[4U * 4U + 0U], 0U);
    EXPECT_EQ(result.value().bytes()[4U * 4U + 1U], 127U);
    EXPECT_EQ(result.value().bytes()[4U * 4U + 2U], 126U);
    EXPECT_EQ(result.value().bytes()[4U * 4U + 3U], 255U);
}

}  // namespace
