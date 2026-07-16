#include "fakes/ManualPushCaptureSource.h"

#include "capture/IVideoFrameSink.h"
#include "core/AppError.h"
#include "domain/Identifiers.h"
#include "media/MediaTypes.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

using creator::capture::CaptureConfig;
using creator::capture::IVideoFrameSink;
using creator::core::AppError;
using creator::core::ErrorCode;
using creator::core::ProjectClock;
using creator::core::TimestampNs;
using creator::fakes::ManualPushCaptureSource;
using creator::media::VideoFrame;

class SinkSpy final : public IVideoFrameSink {
public:
    void onCaptureStarted() noexcept override { ++startCalls; }

    void onVideoFrame(VideoFrame frame) noexcept override {
        ++frameCalls;
        lastFrame = std::move(frame);
    }

    void onCaptureError(AppError error) noexcept override {
        ++errorCalls;
        lastError = std::move(error);
    }

    int startCalls{0};
    int frameCalls{0};
    int errorCalls{0};
    std::optional<VideoFrame> lastFrame;
    std::optional<AppError> lastError;
};

VideoFrame frameAt(std::int64_t nanoseconds) {
    return VideoFrame{.timestamp = TimestampNs{ProjectClock::duration{nanoseconds}},
                      .width = 1280,
                      .height = 720};
}

std::unique_ptr<ManualPushCaptureSource> makeSource(const std::shared_ptr<SinkSpy>& sink) {
    return std::make_unique<ManualPushCaptureSource>(
        creator::domain::SourceId::create("manual-screen").value(), "Manual Screen", sink);
}

TEST(ManualPushCaptureSourceTest, PushesFramesOnlyWhileStarted) {
    auto sink = std::make_shared<SinkSpy>();
    auto source = makeSource(sink);

    EXPECT_FALSE(source->pushFrame(frameAt(1)).hasValue());
    ASSERT_TRUE(source->start(CaptureConfig{}).hasValue());
    EXPECT_EQ(sink->startCalls, 1);
    ASSERT_TRUE(source->pushFrame(frameAt(2)).hasValue());
    ASSERT_TRUE(source->stop().hasValue());
    EXPECT_FALSE(source->pushFrame(frameAt(3)).hasValue());

    ASSERT_EQ(sink->frameCalls, 1);
    ASSERT_TRUE(sink->lastFrame.has_value());
    EXPECT_EQ(sink->lastFrame->timestamp.time_since_epoch().count(), 2);
}

TEST(ManualPushCaptureSourceTest, TerminalFailureStopsAndNotifiesExactlyOnce) {
    auto sink = std::make_shared<SinkSpy>();
    auto source = makeSource(sink);
    ASSERT_TRUE(source->start(CaptureConfig{}).hasValue());

    ASSERT_TRUE(source->fail(AppError{ErrorCode::NotFound, "window closed"}).hasValue());
    EXPECT_FALSE(source->fail(AppError{ErrorCode::Unknown, "duplicate"}).hasValue());
    EXPECT_FALSE(source->pushFrame(frameAt(4)).hasValue());
    EXPECT_TRUE(source->stop().hasValue());

    ASSERT_EQ(sink->errorCalls, 1);
    ASSERT_TRUE(sink->lastError.has_value());
    EXPECT_EQ(sink->lastError->code(), ErrorCode::NotFound);
    EXPECT_EQ(source->stats().receivedFrames, 0u);
}

TEST(ManualPushCaptureSourceTest, CountsDeliveredFramesAndStopIsIdempotent) {
    auto sink = std::make_shared<SinkSpy>();
    auto source = makeSource(sink);
    ASSERT_TRUE(source->start(CaptureConfig{}).hasValue());
    ASSERT_TRUE(source->pushFrame(frameAt(1)).hasValue());
    ASSERT_TRUE(source->pushFrame(frameAt(2)).hasValue());

    EXPECT_TRUE(source->stop().hasValue());
    EXPECT_TRUE(source->stop().hasValue());
    EXPECT_EQ(source->stats().receivedFrames, 2u);
}

}  // namespace
