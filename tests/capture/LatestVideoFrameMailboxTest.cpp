#include "capture/LatestVideoFrameMailbox.h"

#include "core/AppError.h"
#include "media/MediaTypes.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <thread>

namespace {

using creator::capture::LatestVideoFrameMailbox;
using creator::core::AppError;
using creator::core::ErrorCode;
using creator::core::ProjectClock;
using creator::core::TimestampNs;
using creator::media::ColorSpace;
using creator::media::PixelFormat;
using creator::media::VideoFrame;

VideoFrame frameAt(std::int64_t nanoseconds, std::shared_ptr<void> handle = {}) {
    return VideoFrame{
        .timestamp = TimestampNs{ProjectClock::duration{nanoseconds}},
        .width = 1920,
        .height = 1080,
        .pixelFormat = PixelFormat::Bgra8,
        .colorSpace = ColorSpace::Rec709Sdr,
        .platformHandle = std::move(handle),
    };
}

TEST(LatestVideoFrameMailboxTest, KeepsOnlyNewestPendingFrame) {
    LatestVideoFrameMailbox mailbox;

    mailbox.onVideoFrame(frameAt(10));
    mailbox.onVideoFrame(frameAt(20));
    mailbox.onVideoFrame(frameAt(30));

    const auto frame = mailbox.takeLatest();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->timestamp.time_since_epoch().count(), 30);
    EXPECT_FALSE(mailbox.takeLatest().has_value());
    const auto stats = mailbox.stats();
    EXPECT_EQ(stats.publishedFrames, 3u);
    EXPECT_EQ(stats.replacedFrames, 2u);
}

TEST(LatestVideoFrameMailboxTest, ReleasesReplacedPlatformHandleExactlyOnce) {
    LatestVideoFrameMailbox mailbox;
    std::atomic<int> releases{0};
    auto retainedHandle = [&releases]() {
        return std::shared_ptr<void>{new int{42}, [&releases](void* value) {
                                         delete static_cast<int*>(value);
                                         ++releases;
                                     }};
    };

    mailbox.onVideoFrame(frameAt(10, retainedHandle()));
    EXPECT_EQ(releases.load(), 0);
    mailbox.onVideoFrame(frameAt(20, retainedHandle()));
    EXPECT_EQ(releases.load(), 1);

    auto consumed = mailbox.takeLatest();
    ASSERT_TRUE(consumed.has_value());
    EXPECT_EQ(releases.load(), 1);
    consumed.reset();
    EXPECT_EQ(releases.load(), 2);
}

TEST(LatestVideoFrameMailboxTest, PreservesFirstTerminalErrorAndRejectsLaterFrames) {
    LatestVideoFrameMailbox mailbox;
    mailbox.onCaptureError(AppError{ErrorCode::NotFound, "captured window closed"});
    mailbox.onCaptureError(AppError{ErrorCode::Unknown, "late duplicate"});
    mailbox.onVideoFrame(frameAt(10));

    const auto error = mailbox.takeError();
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code(), ErrorCode::NotFound);
    EXPECT_EQ(error->message(), "captured window closed");
    EXPECT_FALSE(mailbox.takeError().has_value());
    EXPECT_FALSE(mailbox.takeLatest().has_value());
    const auto stats = mailbox.stats();
    EXPECT_EQ(stats.terminalErrors, 2u);
    EXPECT_EQ(stats.framesAfterTerminalError, 1u);
}

TEST(LatestVideoFrameMailboxTest, ProducerAndConsumerCoordinateWithoutPollingOrSleep) {
    LatestVideoFrameMailbox mailbox;
    std::promise<void> firstPublishedPromise;
    auto firstPublished = firstPublishedPromise.get_future();
    std::promise<void> firstTakenPromise;
    auto firstTaken = firstTakenPromise.get_future();
    std::optional<VideoFrame> first;
    std::optional<VideoFrame> second;

    std::thread producer{[&] {
        mailbox.onVideoFrame(frameAt(1));
        firstPublishedPromise.set_value();
        firstTaken.wait();
        mailbox.onVideoFrame(frameAt(2));
    }};
    std::thread consumer{[&] {
        firstPublished.wait();
        first = mailbox.takeLatest();
        firstTakenPromise.set_value();
    }};

    producer.join();
    consumer.join();
    second = mailbox.takeLatest();

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->timestamp.time_since_epoch().count(), 1);
    EXPECT_EQ(second->timestamp.time_since_epoch().count(), 2);
    EXPECT_EQ(mailbox.stats().replacedFrames, 0u);
}

}  // namespace

