#include "avatar/inochi2d/Inochi2dAvatarRenderer.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using creator::avatar::AvatarParameterValue;
using creator::avatar::inochi2d::Inochi2dAvatarRenderer;
using creator::core::ErrorCode;
using creator::core::TimestampNs;

TEST(Inochi2dAvatarRendererTest, ReportsMissingRuntimeAsInvalidState) {
    Inochi2dAvatarRenderer renderer{nullptr, 4, 4};
    const auto result = renderer.render(TimestampNs{}, std::vector<AvatarParameterValue>{});
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
}

}  // namespace
