#include "avatar/AvatarRenderPipeline.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <span>
#include <vector>

namespace {

using creator::avatar::AvatarMotionSample;
using creator::avatar::AvatarParameterMapper;
using creator::avatar::AvatarParameterSource;
using creator::avatar::AvatarRenderFrame;
using creator::avatar::AvatarRenderPipeline;
using creator::avatar::IAvatarRenderer;
using creator::avatar::AvatarProviderId;
using creator::avatar::ExpressionParameters;
using creator::core::DurationNs;
using creator::core::Result;
using creator::core::TimestampNs;

class FakeAvatarRenderer final : public IAvatarRenderer {
public:
    Result<AvatarRenderFrame> render(
        TimestampNs timestamp,
        std::span<const creator::avatar::AvatarParameterValue> parameters) override {
        received_.assign(parameters.begin(), parameters.end());
        return AvatarRenderFrame::transparent(timestamp, 4, 2);
    }

    const auto& received() const noexcept { return received_; }

private:
    std::vector<creator::avatar::AvatarParameterValue> received_;
};

TEST(AvatarRenderPipelineTest, MapsMotionIntoTransparentTimestampedFrame) {
    const auto mapper = AvatarParameterMapper::create({
        {"Mouth", AvatarParameterSource::MouthOpen, 1.0F, 0.0F, 0.0F, 1.0F},
    });
    ASSERT_TRUE(mapper.hasValue()) << mapper.error().message();
    FakeAvatarRenderer renderer;
    AvatarRenderPipeline pipeline{mapper.value(), renderer};
    const auto provider = AvatarProviderId::create("fake").value();
    const AvatarMotionSample sample{
        TimestampNs{DurationNs{77'000'000}}, ExpressionParameters{.mouthOpen = 0.75F}, provider};

    const auto frame = pipeline.render(sample);
    ASSERT_TRUE(frame.hasValue()) << frame.error().message();
    EXPECT_EQ(frame.value().timestamp(), sample.timestamp);
    EXPECT_EQ(frame.value().width(), 4U);
    EXPECT_EQ(frame.value().height(), 2U);
    EXPECT_EQ(frame.value().bytes().size(), 32U);
    EXPECT_TRUE(std::all_of(frame.value().bytes().begin(), frame.value().bytes().end(),
                            [](std::uint8_t byte) {
        return byte == 0;
    }));
    ASSERT_EQ(renderer.received().size(), 1U);
    EXPECT_FLOAT_EQ(renderer.received()[0].value, 0.75F);
}

TEST(AvatarRenderFrameTest, RejectsMismatchedBgraStorage) {
    const auto result = AvatarRenderFrame::fromBgra(TimestampNs{}, 2, 2, 8, {1, 2, 3});
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), creator::core::ErrorCode::InvalidArgument);
}

}  // namespace
