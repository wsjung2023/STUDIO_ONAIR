#include "avatar/AvatarParameterMapper.h"

#include <gtest/gtest.h>

#include <limits>
#include <vector>

namespace {

using creator::avatar::AvatarParameterBinding;
using creator::avatar::AvatarParameterMapper;
using creator::avatar::AvatarParameterSource;
using creator::avatar::ExpressionParameters;
using creator::core::ErrorCode;

TEST(AvatarParameterMapperTest, AppliesScaleOffsetAndClampsInBindingOrder) {
    auto mapper = AvatarParameterMapper::create({
        {"ParamMouth", AvatarParameterSource::MouthOpen, 2.0F, -0.2F, 0.0F, 1.0F},
        {"ParamYaw", AvatarParameterSource::HeadYaw, 0.5F, 0.5F, 0.0F, 1.0F},
    });
    ASSERT_TRUE(mapper.hasValue()) << mapper.error().message();
    const auto values = mapper.value().map(ExpressionParameters{
        .mouthOpen = 0.8F,
        .headYaw = -1.0F,
    });
    ASSERT_TRUE(values.hasValue()) << values.error().message();
    ASSERT_EQ(values.value().size(), 2U);
    EXPECT_EQ(values.value()[0].modelParameter, "ParamMouth");
    EXPECT_FLOAT_EQ(values.value()[0].value, 1.0F);
    EXPECT_FLOAT_EQ(values.value()[1].value, 0.0F);
}

TEST(AvatarParameterMapperTest, RejectsDuplicateNamesAndInvalidRanges) {
    const auto duplicate = AvatarParameterMapper::create({
        {"Param", AvatarParameterSource::MouthOpen},
        {"Param", AvatarParameterSource::MouthWide},
    });
    ASSERT_FALSE(duplicate.hasValue());
    EXPECT_EQ(duplicate.error().code(), ErrorCode::AlreadyExists);

    const auto invalidRange = AvatarParameterMapper::create({
        {"Param", AvatarParameterSource::MouthOpen, 1.0F, 0.0F, 1.0F, 0.0F},
    });
    ASSERT_FALSE(invalidRange.hasValue());
    EXPECT_EQ(invalidRange.error().code(), ErrorCode::InvalidArgument);
}

TEST(AvatarParameterMapperTest, RejectsNonFiniteExpressions) {
    auto mapper = AvatarParameterMapper::create({
        {"ParamMouth", AvatarParameterSource::MouthOpen},
    });
    ASSERT_TRUE(mapper.hasValue());
    ExpressionParameters parameters{};
    parameters.mouthOpen = std::numeric_limits<float>::quiet_NaN();
    const auto values = mapper.value().map(parameters);
    ASSERT_FALSE(values.hasValue());
    EXPECT_EQ(values.error().code(), ErrorCode::InvalidArgument);
}

}  // namespace
