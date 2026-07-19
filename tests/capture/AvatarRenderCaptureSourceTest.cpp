#include "capture/AvatarRenderCaptureSource.h"

#include "domain/Identifiers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

namespace {

using creator::capture::AvatarRenderCaptureSource;
using creator::capture::CaptureConfig;
using creator::core::AppError;
using creator::core::ErrorCode;
using creator::core::Result;
using creator::core::TimestampNs;
using creator::domain::SourceId;
using creator::media::ColorSpace;
using creator::media::PixelFormat;
using creator::media::VideoFrame;

SourceId sourceId() { return SourceId::create("avatar-render").value(); }

TEST(AvatarRenderCaptureSourceTest, RejectsTickBeforeStart) {
    AvatarRenderCaptureSource source{sourceId(), "Avatar", [](TimestampNs timestamp) {
        VideoFrame frame;
        frame.timestamp = timestamp;
        return Result<VideoFrame>{frame};
    }};
    const auto result = source.tick();
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
}

TEST(AvatarRenderCaptureSourceTest, RendersConfiguredFrameRateWithExactTimestamps) {
    std::vector<TimestampNs> requested;
    AvatarRenderCaptureSource source{
        sourceId(), "Avatar", [&requested](TimestampNs timestamp) {
            requested.push_back(timestamp);
            VideoFrame frame;
            frame.timestamp = timestamp;
            frame.width = 1280;
            frame.height = 720;
            frame.pixelFormat = PixelFormat::Bgra8;
            frame.colorSpace = ColorSpace::Rec709Sdr;
            return Result<VideoFrame>{frame};
        }};
    ASSERT_TRUE(source.start(CaptureConfig{1280, 720, 30, 1}).hasValue());
    const auto first = source.tick();
    const auto second = source.tick();
    ASSERT_TRUE(first.hasValue());
    ASSERT_TRUE(second.hasValue());
    EXPECT_EQ(first.value().timestamp, TimestampNs{});
    EXPECT_EQ(second.value().timestamp,
              TimestampNs{std::chrono::nanoseconds{33'333'334}});
    EXPECT_EQ(requested, (std::vector<TimestampNs>{first.value().timestamp,
                                                   second.value().timestamp}));
    EXPECT_EQ(source.stats().receivedFrames, 2U);
}

TEST(AvatarRenderCaptureSourceTest, PropagatesRendererFailureWithoutAdvancing) {
    AvatarRenderCaptureSource source{sourceId(), "Avatar", [](TimestampNs) {
        return Result<VideoFrame>{AppError{ErrorCode::IoFailure,
                                           "avatar renderer failed"}};
    }};
    ASSERT_TRUE(source.start(CaptureConfig{1, 1, 60, 1}).hasValue());
    const auto result = source.tick();
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::IoFailure);
    EXPECT_EQ(source.stats().receivedFrames, 0U);
    EXPECT_EQ(source.tick().error().code(), ErrorCode::IoFailure);
}

TEST(AvatarRenderCaptureSourceTest, RejectsRendererTimestampMismatch) {
    AvatarRenderCaptureSource source{sourceId(), "Avatar", [](TimestampNs) {
        VideoFrame frame;
        frame.timestamp = TimestampNs{std::chrono::nanoseconds{1}};
        return Result<VideoFrame>{frame};
    }};
    ASSERT_TRUE(source.start(CaptureConfig{1, 1, 60, 1}).hasValue());
    const auto result = source.tick();
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

}  // namespace
