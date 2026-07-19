#include "capture/CaptureFanoutSinks.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "media/MediaTypes.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace {

using creator::capture::AudioBlockFanoutSink;
using creator::capture::IAudioBlockSink;
using creator::capture::IVideoFrameSink;
using creator::capture::VideoFrameFanoutSink;
using creator::core::AppError;
using creator::core::ErrorCode;
using creator::media::AudioBlock;
using creator::media::VideoFrame;

class VideoSink final : public IVideoFrameSink {
public:
    void onCaptureStarted() noexcept override { ++starts; }
    void onVideoFrame(VideoFrame frame) noexcept override {
        timestamps.push_back(frame.timestamp.time_since_epoch().count());
        lastHandle = std::move(frame.platformHandle);
    }
    void onCaptureError(AppError error) noexcept override {
        terminalError = std::move(error);
    }

    int starts{0};
    std::vector<std::int64_t> timestamps;
    std::shared_ptr<void> lastHandle;
    std::optional<AppError> terminalError;
};

class AudioSink final : public IAudioBlockSink {
public:
    void onCaptureStarted() noexcept override { ++starts; }
    void onAudioBlock(AudioBlock block) noexcept override {
        timestamps.push_back(block.timestamp.time_since_epoch().count());
        lastSamples = std::move(block.samples);
    }
    void onCaptureError(AppError error) noexcept override {
        terminalError = std::move(error);
    }

    int starts{0};
    std::vector<std::int64_t> timestamps;
    std::shared_ptr<const float[]> lastSamples;
    std::optional<AppError> terminalError;
};

VideoFrame videoAt(std::int64_t timestamp, std::shared_ptr<void> handle = {}) {
    VideoFrame frame;
    frame.timestamp = creator::core::TimestampNs{creator::core::Nanoseconds{timestamp}};
    frame.width = 2;
    frame.height = 2;
    frame.platformHandle = std::move(handle);
    return frame;
}

AudioBlock audioAt(std::int64_t timestamp) {
    auto samples = std::shared_ptr<float[]>(new float[1]{0.25F},
                                            std::default_delete<float[]>{});
    AudioBlock block;
    block.timestamp = creator::core::TimestampNs{creator::core::Nanoseconds{timestamp}};
    block.channels = 1;
    block.frameCount = 1;
    block.samples = std::move(samples);
    return block;
}

TEST(VideoFrameFanoutSinkTest, KeepsPreviewAliveWhileRecordingSinkAttachesAndDetaches) {
    auto preview = std::make_shared<VideoSink>();
    auto recording = std::make_shared<VideoSink>();
    VideoFrameFanoutSink fanout{preview};

    fanout.onCaptureStarted();
    fanout.onVideoFrame(videoAt(1));
    fanout.setSecondary(recording);
    fanout.onVideoFrame(videoAt(2));
    fanout.setSecondary({});
    fanout.onVideoFrame(videoAt(3));

    EXPECT_EQ(preview->starts, 1);
    EXPECT_EQ(preview->timestamps, (std::vector<std::int64_t>{1, 2, 3}));
    EXPECT_EQ(recording->timestamps, (std::vector<std::int64_t>{2}));
}

TEST(VideoFrameFanoutSinkTest, SharesPlatformHandleAndForwardsTerminalError) {
    auto preview = std::make_shared<VideoSink>();
    auto recording = std::make_shared<VideoSink>();
    VideoFrameFanoutSink fanout{preview};
    fanout.setSecondary(recording);
    auto handle = std::shared_ptr<void>{new int{42}, [](void* value) {
                                            delete static_cast<int*>(value);
                                        }};

    fanout.onCaptureStarted();
    fanout.onVideoFrame(videoAt(7, handle));
    fanout.onCaptureError({ErrorCode::IoFailure, "capture failed"});

    EXPECT_EQ(recording->starts, 1);
    EXPECT_EQ(preview->lastHandle, handle);
    EXPECT_EQ(recording->lastHandle, handle);
    ASSERT_TRUE(preview->terminalError.has_value());
    ASSERT_TRUE(recording->terminalError.has_value());
    EXPECT_EQ(preview->terminalError->message(), "capture failed");
    EXPECT_EQ(recording->terminalError->message(), "capture failed");
}

TEST(AudioBlockFanoutSinkTest, SharesSamplesAndCanDetachRecordingSink) {
    auto meter = std::make_shared<AudioSink>();
    auto recording = std::make_shared<AudioSink>();
    AudioBlockFanoutSink fanout{meter};
    fanout.setSecondary(recording);

    fanout.onCaptureStarted();
    fanout.onAudioBlock(audioAt(11));
    EXPECT_EQ(meter->lastSamples, recording->lastSamples);
    fanout.setSecondary({});
    fanout.onAudioBlock(audioAt(12));
    fanout.onCaptureError({ErrorCode::NotFound, "device removed"});

    EXPECT_EQ(meter->timestamps, (std::vector<std::int64_t>{11, 12}));
    EXPECT_EQ(recording->timestamps, (std::vector<std::int64_t>{11}));
    ASSERT_TRUE(meter->terminalError.has_value());
    EXPECT_FALSE(recording->terminalError.has_value());
}

}  // namespace
