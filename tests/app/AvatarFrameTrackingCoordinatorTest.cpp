#include "app/AvatarFrameTrackingCoordinator.h"
#include "avatar/AvatarMotionNdjsonSink.h"
#include "avatar/AvatarProviderId.h"
#include "avatar/CalibrationProfile.h"
#include "fakes/FakeTrackingProvider.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>

namespace {

using creator::app::AvatarFrameTrackingCoordinator;
using creator::avatar::AvatarMotionNdjsonSink;
using creator::avatar::AvatarMotionPipeline;
using creator::avatar::AvatarProviderId;
using creator::avatar::CalibrationProfile;
using creator::avatar::ExpressionParameters;
using creator::capture::LatestVideoFrameMailbox;
using creator::core::AppError;
using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::TimestampNs;
using creator::fakes::FakeTrackingProvider;
using creator::media::VideoFrame;

TEST(AvatarFrameTrackingCoordinatorTest, ConsumesLatestCameraFrameAndPreservesTimestamp) {
    const auto root = std::filesystem::temp_directory_path() /
                      "creator-studio-avatar-frame-coordinator";
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    std::filesystem::create_directories(root / "telemetry");

    AvatarMotionNdjsonSink sink{root / "telemetry"};
    const auto providerId = AvatarProviderId::create("fake").value();
    AvatarMotionPipeline pipeline{providerId, CalibrationProfile::identity(), sink};
    FakeTrackingProvider provider{
        {FakeTrackingProvider::ScriptedFrame{
            .parameters = ExpressionParameters{.mouthOpen = 0.65F}}},
        providerId};
    auto mailbox = std::make_shared<LatestVideoFrameMailbox>();
    VideoFrame frame{};
    frame.timestamp = TimestampNs{DurationNs{123'000'000}};
    mailbox->onVideoFrame(frame);

    AvatarFrameTrackingCoordinator coordinator{mailbox, provider, pipeline};
    const auto sample = coordinator.poll();
    ASSERT_TRUE(sample.hasValue()) << sample.error().message();
    ASSERT_TRUE(sample.value().has_value());
    EXPECT_EQ(sample.value()->timestamp, frame.timestamp);
    EXPECT_FLOAT_EQ(sample.value()->parameters.mouthOpen, 0.65F);
    const auto emptyPoll = coordinator.poll();
    ASSERT_TRUE(emptyPoll.hasValue()) << emptyPoll.error().message();
    EXPECT_FALSE(emptyPoll.value().has_value());

    std::filesystem::remove_all(root, cleanupError);
}

TEST(AvatarFrameTrackingCoordinatorTest, ReportsTerminalCaptureErrorBeforeFrames) {
    const auto root = std::filesystem::temp_directory_path() /
                      "creator-studio-avatar-frame-coordinator-error";
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    std::filesystem::create_directories(root / "telemetry");
    AvatarMotionNdjsonSink sink{root / "telemetry"};
    const auto providerId = AvatarProviderId::create("fake").value();
    AvatarMotionPipeline pipeline{providerId, CalibrationProfile::identity(), sink};
    FakeTrackingProvider provider{{}, providerId};
    auto mailbox = std::make_shared<LatestVideoFrameMailbox>();
    mailbox->onCaptureError(AppError{ErrorCode::IoFailure, "camera stopped"});

    AvatarFrameTrackingCoordinator coordinator{mailbox, provider, pipeline};
    const auto result = coordinator.poll();
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::IoFailure);
    std::filesystem::remove_all(root, cleanupError);
}

}  // namespace
