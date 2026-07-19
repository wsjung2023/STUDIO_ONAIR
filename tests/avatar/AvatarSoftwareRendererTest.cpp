#include "avatar/AvatarSoftwareRenderer.h"

#include "avatar/AvatarParameterMapper.h"
#include "avatar/AvatarRenderPipeline.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using creator::avatar::AvatarMeshVertex;
using creator::avatar::AvatarParameterMapper;
using creator::avatar::AvatarParameterSource;
using creator::avatar::AvatarSoftwareRenderInput;
using creator::avatar::AvatarSoftwareRenderer;
using creator::avatar::AvatarMotionSample;
using creator::avatar::AvatarRenderPipeline;
using creator::avatar::ExpressionParameters;
using creator::core::DurationNs;
using creator::core::TimestampNs;

TEST(AvatarSoftwareRendererTest, ConnectsMappedMotionToRasterizedVideoFrame) {
    std::vector<float> mouthValues;
    AvatarSoftwareRenderer renderer{4, 4, [&mouthValues](
                                      TimestampNs,
                                      std::span<const creator::avatar::AvatarParameterValue>
                                          parameters) {
        mouthValues.push_back(parameters.front().value);
        return creator::core::Result<AvatarSoftwareRenderInput>{
            AvatarSoftwareRenderInput{
                {{0.0F, 0.0F, 0.0F, 0.0F},
                 {4.0F, 0.0F, 1.0F, 0.0F},
                 {0.0F, 4.0F, 0.0F, 1.0F}},
                {0, 1, 2},
                {1, 1, {8, 16, 24, 255}}}};
    }};
    const auto mapper = AvatarParameterMapper::create({
        {"Mouth", AvatarParameterSource::MouthOpen, 1.0F, 0.0F, 0.0F, 1.0F},
    });
    ASSERT_TRUE(mapper.hasValue());
    AvatarRenderPipeline pipeline{mapper.value(), renderer};
    const auto provider = creator::avatar::AvatarProviderId::create("fake").value();
    const auto frame = pipeline.render(AvatarMotionSample{
        TimestampNs{DurationNs{90}}, ExpressionParameters{.mouthOpen = 0.75F}, provider});
    ASSERT_TRUE(frame.hasValue()) << frame.error().message();
    EXPECT_EQ(mouthValues, std::vector<float>{0.75F});
    EXPECT_EQ(frame.value().bytes()[4U * 4U + 0U], 8U);
    EXPECT_EQ(frame.value().bytes()[4U * 4U + 1U], 16U);
    EXPECT_EQ(frame.value().bytes()[4U * 4U + 2U], 24U);
    EXPECT_EQ(frame.value().bytes()[4U * 4U + 3U], 255U);
}

TEST(AvatarSoftwareRendererTest, RejectsMissingMeshProvider) {
    AvatarSoftwareRenderer renderer{4, 4, {}};
    const auto result = renderer.render(TimestampNs{}, {});
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), creator::core::ErrorCode::InvalidState);
}

}  // namespace
