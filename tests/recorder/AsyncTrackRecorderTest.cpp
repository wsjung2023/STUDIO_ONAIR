#include "recorder/AsyncTrackRecorder.h"

#include "core/AppError.h"
#include "domain/Identifiers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace {

using creator::core::AppError;
using creator::core::ErrorCode;
using creator::core::Result;
using creator::domain::SegmentInfo;
using creator::domain::SourceId;
using creator::media::AudioBlock;
using creator::media::VideoFrame;
using creator::recorder::AsyncTrackRecorder;
using creator::recorder::AsyncTrackRecorderConfig;
using creator::recorder::DiskSpaceMonitor;
using creator::recorder::DiskSpaceValues;
using creator::recorder::DurableSegmentPublisher;
using creator::recorder::EncodedSegment;
using creator::recorder::IDiskSpaceProbe;
using creator::recorder::ISegmentFileOperations;
using creator::recorder::ISegmentLifecycleSink;
using creator::recorder::ITrackSegmentEncoder;
using creator::recorder::RecordingTrack;
using creator::recorder::SegmentEncodeConfig;
using creator::recorder::TrackRecordingSummary;
using creator::recorder::TrackRole;

class SpaceProbe final : public IDiskSpaceProbe {
public:
    Result<DiskSpaceValues> query(const std::filesystem::path&) override {
        return DiskSpaceValues{.capacityBytes = 1ULL << 40U,
                               .freeBytes = 1ULL << 40U,
                               .availableBytes = 1ULL << 40U};
    }
};

class FileOperations final : public ISegmentFileOperations {
public:
    Result<void> prepare(const std::filesystem::path&,
                         const std::filesystem::path&) override {
        return creator::core::ok();
    }
    Result<void> publish(const std::filesystem::path&,
                         const std::filesystem::path&) override {
        return creator::core::ok();
    }
    bool didPublishLastAttempt(const std::filesystem::path&,
                               const std::filesystem::path&) const noexcept override {
        return true;
    }
};

struct LifecycleState final {
    std::mutex mutex;
    std::vector<SegmentInfo> began;
    std::vector<SegmentInfo> ready;
    std::uint64_t failedCalls{0};
};

class LifecycleSink final : public ISegmentLifecycleSink {
public:
    explicit LifecycleSink(std::shared_ptr<LifecycleState> state) : state_(std::move(state)) {}

    Result<void> begin(const SegmentInfo& segment) override {
        std::lock_guard lock{state_->mutex};
        state_->began.push_back(segment);
        return creator::core::ok();
    }
    Result<void> ready(const SegmentInfo& segment) override {
        std::lock_guard lock{state_->mutex};
        state_->ready.push_back(segment);
        return creator::core::ok();
    }
    Result<void> failed(const SourceId&, std::uint64_t) override {
        std::lock_guard lock{state_->mutex};
        ++state_->failedCalls;
        return creator::core::ok();
    }

private:
    std::shared_ptr<LifecycleState> state_;
};

struct EncoderState final {
    std::mutex mutex;
    std::condition_variable entered;
    std::condition_variable released;
    bool blockFirstAccept{false};
    bool firstAcceptEntered{false};
    bool releaseFirstAccept{false};
    std::optional<AppError> acceptError;
    std::vector<SegmentEncodeConfig> starts;
    std::vector<creator::core::TimestampNs> finishes;
    std::uint64_t videoAccepts{0};
    std::uint64_t audioFrames{0};
    std::vector<double> audioRateRatios;
};

class Encoder final : public ITrackSegmentEncoder {
public:
    explicit Encoder(std::shared_ptr<EncoderState> state) : state_(std::move(state)) {}

    Result<void> start(const SegmentEncodeConfig& config) override {
        std::lock_guard lock{state_->mutex};
        state_->starts.push_back(config);
        return creator::core::ok();
    }
    Result<void> accept(const VideoFrame&) override { return acceptFrames(1, true, 1.0); }
    Result<void> accept(const AudioBlock& block) override {
        return acceptFrames(block.frameCount, false, block.sampleRateRatio);
    }
    Result<EncodedSegment> finish(creator::core::TimestampNs endTime) override {
        std::lock_guard lock{state_->mutex};
        state_->finishes.push_back(endTime);
        return EncodedSegment{.endTime = endTime, .bytesWritten = 10, .codecName = "fake"};
    }
    void abort() noexcept override {}

private:
    Result<void> acceptFrames(std::uint64_t count, bool video, double rateRatio) {
        std::unique_lock lock{state_->mutex};
        if (state_->blockFirstAccept && !state_->firstAcceptEntered) {
            state_->firstAcceptEntered = true;
            state_->entered.notify_all();
            state_->released.wait(lock, [this] { return state_->releaseFirstAccept; });
        }
        if (state_->acceptError) return *state_->acceptError;
        if (video) ++state_->videoAccepts;
        else {
            state_->audioFrames += count;
            state_->audioRateRatios.push_back(rateRatio);
        }
        return creator::core::ok();
    }

    std::shared_ptr<EncoderState> state_;
};

RecordingTrack track(TrackRole role) {
    return RecordingTrack::create(SourceId::create("source-1").value(), role).value();
}

VideoFrame videoAt(std::chrono::milliseconds timestamp) {
    return VideoFrame{.timestamp = creator::core::TimestampNs{timestamp},
                      .width = 16,
                      .height = 16,
                      .pixelFormat = creator::media::PixelFormat::Bgra8};
}

AudioBlock audioAt(std::chrono::milliseconds timestamp, std::uint32_t frames) {
    auto samples = std::shared_ptr<float[]>(new float[frames * 2]{});
    return AudioBlock{.timestamp = creator::core::TimestampNs{timestamp},
                      .sampleRate = 48'000,
                      .channels = 2,
                      .frameCount = frames,
                      .samples = std::move(samples)};
}

struct RecorderFixture final {
    explicit RecorderFixture(TrackRole role, std::size_t videoCapacity = 3,
                             std::uint64_t audioCapacity = 48'000) {
        encoderState = std::make_shared<EncoderState>();
        lifecycleState = std::make_shared<LifecycleState>();
        auto publisher = std::make_unique<DurableSegmentPublisher>(
            "project.cstudio", std::make_unique<FileOperations>(),
            std::make_unique<LifecycleSink>(lifecycleState));
        AsyncTrackRecorderConfig config{
            .track = track(role),
            .packageRoot = "project.cstudio",
            .recordingStartTime = {},
            .segmentDuration = std::chrono::seconds{2},
            .videoQueueCapacity = videoCapacity,
            .audioQueueFrameCapacity = audioCapacity,
            .nextSegmentEstimateBytes = 1,
        };
        recorder = std::make_unique<AsyncTrackRecorder>(
            std::move(config), std::make_unique<Encoder>(encoderState),
            std::move(publisher),
            std::make_unique<DiskSpaceMonitor>(std::make_unique<SpaceProbe>(), 0));
    }

    std::future<Result<TrackRecordingSummary>> stopAt(std::chrono::milliseconds endTime) {
        auto promise = std::make_shared<std::promise<Result<TrackRecordingSummary>>>();
        auto future = promise->get_future();
        recorder->stopAsync(creator::core::TimestampNs{endTime},
                            [promise](const auto& result) { promise->set_value(result); });
        return future;
    }

    void waitUntilEncoderBlocks() {
        std::unique_lock lock{encoderState->mutex};
        ASSERT_TRUE(encoderState->entered.wait_for(lock, std::chrono::seconds{2}, [this] {
            return encoderState->firstAcceptEntered;
        }));
    }

    void releaseEncoder() {
        std::lock_guard lock{encoderState->mutex};
        encoderState->releaseFirstAccept = true;
        encoderState->released.notify_all();
    }

    std::shared_ptr<EncoderState> encoderState;
    std::shared_ptr<LifecycleState> lifecycleState;
    std::unique_ptr<AsyncTrackRecorder> recorder;
};

TEST(AsyncTrackRecorderTest, VideoQueueKeepsNewestFrameAndCountsDisplacement) {
    RecorderFixture fixture{TrackRole::Screen, 1};
    fixture.encoderState->blockFirstAccept = true;
    ASSERT_TRUE(fixture.recorder->start().hasValue());
    ASSERT_TRUE(fixture.recorder->accept(videoAt(std::chrono::milliseconds{0})).hasValue());
    fixture.waitUntilEncoderBlocks();
    ASSERT_TRUE(fixture.recorder->accept(videoAt(std::chrono::milliseconds{100})).hasValue());
    ASSERT_TRUE(fixture.recorder->accept(videoAt(std::chrono::milliseconds{200})).hasValue());
    auto stopped = fixture.stopAt(std::chrono::milliseconds{500});
    fixture.releaseEncoder();

    const auto result = stopped.get();
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_EQ(result.value().videoFramesAccepted, 2u);
    EXPECT_EQ(result.value().videoFramesDropped, 1u);
}

TEST(AsyncTrackRecorderTest, AudioQueueOverflowIsTerminalAndNeverSilentlyDrops) {
    RecorderFixture fixture{TrackRole::Microphone, 3, 4};
    fixture.encoderState->blockFirstAccept = true;
    ASSERT_TRUE(fixture.recorder->start().hasValue());
    ASSERT_TRUE(fixture.recorder->accept(audioAt(std::chrono::milliseconds{0}, 4)).hasValue());
    fixture.waitUntilEncoderBlocks();
    ASSERT_TRUE(fixture.recorder->accept(audioAt(std::chrono::milliseconds{1}, 4)).hasValue());
    const auto overflow = fixture.recorder->accept(audioAt(std::chrono::milliseconds{2}, 1));
    ASSERT_FALSE(overflow.hasValue());
    auto stopped = fixture.stopAt(std::chrono::milliseconds{10});
    fixture.releaseEncoder();

    const auto result = stopped.get();
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
    EXPECT_EQ(fixture.encoderState->audioFrames, 8u);
}

TEST(AsyncTrackRecorderTest, PreservesAudioRateRatioWhenSplittingAtSegmentBoundary) {
    RecorderFixture fixture{TrackRole::Microphone};
    ASSERT_TRUE(fixture.recorder->start().hasValue());
    auto block = audioAt(std::chrono::milliseconds{1990}, 960);
    block.sampleRateRatio = 1.0005;

    ASSERT_TRUE(fixture.recorder->accept(std::move(block)).hasValue());
    const auto stopped = fixture.stopAt(std::chrono::milliseconds{2010}).get();

    ASSERT_TRUE(stopped.hasValue()) << stopped.error().message();
    std::lock_guard lock{fixture.encoderState->mutex};
    ASSERT_EQ(fixture.encoderState->audioRateRatios.size(), 2u);
    EXPECT_DOUBLE_EQ(fixture.encoderState->audioRateRatios[0], 1.0005);
    EXPECT_DOUBLE_EQ(fixture.encoderState->audioRateRatios[1], 1.0005);
}

TEST(AsyncTrackRecorderTest, PublishesAtProjectTwoSecondBoundariesAndFlushesTail) {
    RecorderFixture fixture{TrackRole::Screen};
    ASSERT_TRUE(fixture.recorder->start().hasValue());
    ASSERT_TRUE(fixture.recorder->accept(videoAt(std::chrono::milliseconds{0})).hasValue());
    ASSERT_TRUE(fixture.recorder->accept(videoAt(std::chrono::milliseconds{1900})).hasValue());
    ASSERT_TRUE(fixture.recorder->accept(videoAt(std::chrono::milliseconds{2000})).hasValue());
    ASSERT_TRUE(fixture.recorder->accept(videoAt(std::chrono::milliseconds{4100})).hasValue());
    const auto result = fixture.stopAt(std::chrono::milliseconds{4500}).get();

    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_EQ(result.value().segmentsPublished, 3u);
    const auto snapshot = fixture.recorder->snapshot();
    ASSERT_TRUE(snapshot.diskSpace.has_value());
    EXPECT_EQ(snapshot.diskSpace->availableBytes, 1ULL << 40U);
    EXPECT_EQ(snapshot.encoderName, "fake");
    std::lock_guard lock{fixture.encoderState->mutex};
    ASSERT_EQ(fixture.encoderState->starts.size(), 3u);
    ASSERT_EQ(fixture.encoderState->finishes.size(), 3u);
    EXPECT_EQ(fixture.encoderState->starts[0].startTime,
              creator::core::TimestampNs{std::chrono::seconds{0}});
    EXPECT_EQ(fixture.encoderState->starts[1].startTime,
              creator::core::TimestampNs{std::chrono::seconds{2}});
    EXPECT_EQ(fixture.encoderState->starts[2].startTime,
              creator::core::TimestampNs{std::chrono::seconds{4}});
    EXPECT_EQ(fixture.encoderState->finishes[0],
              creator::core::TimestampNs{std::chrono::seconds{2}});
    EXPECT_EQ(fixture.encoderState->finishes[1],
              creator::core::TimestampNs{std::chrono::seconds{4}});
    EXPECT_EQ(fixture.encoderState->finishes[2],
              creator::core::TimestampNs{std::chrono::milliseconds{4500}});
}

TEST(AsyncTrackRecorderTest, WorkerFailureFailsPendingSegmentAndCompletesAllStops) {
    RecorderFixture fixture{TrackRole::Screen};
    fixture.encoderState->blockFirstAccept = true;
    fixture.encoderState->acceptError = AppError{ErrorCode::IoFailure, "encoder failed"};
    ASSERT_TRUE(fixture.recorder->start().hasValue());
    ASSERT_TRUE(fixture.recorder->accept(videoAt(std::chrono::milliseconds{0})).hasValue());
    fixture.waitUntilEncoderBlocks();
    auto first = fixture.stopAt(std::chrono::milliseconds{100});
    auto second = fixture.stopAt(std::chrono::milliseconds{100});
    fixture.releaseEncoder();

    ASSERT_FALSE(first.get().hasValue());
    ASSERT_FALSE(second.get().hasValue());
    std::lock_guard lock{fixture.lifecycleState->mutex};
    EXPECT_EQ(fixture.lifecycleState->failedCalls, 1u);
}

TEST(AsyncTrackRecorderTest, RecorderCannotRestartAfterStop) {
    RecorderFixture fixture{TrackRole::Screen};
    ASSERT_TRUE(fixture.recorder->start().hasValue());
    ASSERT_TRUE(fixture.stopAt(std::chrono::milliseconds{0}).get().hasValue());
    const auto restarted = fixture.recorder->start();
    ASSERT_FALSE(restarted.hasValue());
    EXPECT_EQ(restarted.error().code(), ErrorCode::InvalidState);
}

TEST(AsyncTrackRecorderTest, DestructorDrainsAcceptedMediaAndPublishesTail) {
    RecorderFixture fixture{TrackRole::Screen};
    ASSERT_TRUE(fixture.recorder->start().hasValue());
    ASSERT_TRUE(fixture.recorder->accept(videoAt(std::chrono::milliseconds{0})).hasValue());

    fixture.recorder.reset();

    std::lock_guard lock{fixture.lifecycleState->mutex};
    ASSERT_EQ(fixture.lifecycleState->ready.size(), 1u);
    EXPECT_EQ(fixture.lifecycleState->ready.front().duration, std::chrono::nanoseconds{1});
    EXPECT_EQ(fixture.lifecycleState->failedCalls, 0u);
}

TEST(AsyncTrackRecorderTest, CompletionObserverSeesWorkerFailureWithoutExplicitStop) {
    RecorderFixture fixture{TrackRole::Screen};
    fixture.encoderState->acceptError = AppError{ErrorCode::IoFailure, "worker failed"};
    auto promise = std::make_shared<std::promise<Result<TrackRecordingSummary>>>();
    auto future = promise->get_future();
    fixture.recorder->observeCompletion(
        [promise](const auto& result) { promise->set_value(result); });
    ASSERT_TRUE(fixture.recorder->start().hasValue());
    ASSERT_TRUE(fixture.recorder->accept(videoAt(std::chrono::milliseconds{0})).hasValue());

    const auto result = future.get();

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().message(), "worker failed");
}

}  // namespace
