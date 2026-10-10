#include "app/MultiTrackRecordingService.h"

#include "domain/Identifiers.h"
#include "recorder/DiskSpaceMonitor.h"
#include "recorder/DurableSegmentPublisher.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace {

using creator::app::MultiTrackRecordingService;
using creator::app::MultiTrackRecordingSummary;
using creator::core::AppError;
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

class LifecycleSink final : public ISegmentLifecycleSink {
public:
    Result<void> begin(const SegmentInfo&) override { return creator::core::ok(); }
    Result<void> ready(const SegmentInfo&) override { return creator::core::ok(); }
    Result<void> failed(const SourceId&, std::uint64_t) override {
        return creator::core::ok();
    }
};

struct EncoderState final {
    std::mutex mutex;
    std::optional<AppError> acceptError;
};

class Encoder final : public ITrackSegmentEncoder {
public:
    explicit Encoder(std::shared_ptr<EncoderState> state) : state_(std::move(state)) {}
    Result<void> start(const SegmentEncodeConfig&) override { return creator::core::ok(); }
    Result<void> accept(const VideoFrame&) override { return accepted(); }
    Result<void> accept(const AudioBlock&) override { return accepted(); }
    Result<EncodedSegment> finish(creator::core::TimestampNs endTime) override {
        return EncodedSegment{.endTime = endTime, .bytesWritten = 10, .codecName = "fake"};
    }
    void abort() noexcept override {}

private:
    Result<void> accepted() {
        std::lock_guard lock{state_->mutex};
        if (state_->acceptError) return *state_->acceptError;
        return creator::core::ok();
    }
    std::shared_ptr<EncoderState> state_;
};

struct BuiltTrack final {
    std::unique_ptr<AsyncTrackRecorder> recorder;
    std::shared_ptr<EncoderState> encoder;
};

BuiltTrack buildTrack(const SourceId& sourceId, TrackRole role,
                      std::size_t videoQueueCapacity = 2) {
    auto state = std::make_shared<EncoderState>();
    auto publisher = std::make_unique<DurableSegmentPublisher>(
        "project.cstudio", "take-1", std::make_unique<FileOperations>(),
        std::make_unique<LifecycleSink>());
    AsyncTrackRecorderConfig config{
        .track = RecordingTrack::create(sourceId, role).value(),
        .packageRoot = "project.cstudio",
        .recordingStartTime = {},
        .segmentDuration = std::chrono::seconds{2},
        .videoQueueCapacity = videoQueueCapacity,
        .audioQueueFrameCapacity = 48'000,
        .nextSegmentEstimateBytes = 1,
    };
    return BuiltTrack{
        .recorder = std::make_unique<AsyncTrackRecorder>(
            std::move(config), std::make_unique<Encoder>(state), std::move(publisher),
            std::make_unique<DiskSpaceMonitor>(std::make_unique<SpaceProbe>(), 0)),
        .encoder = std::move(state),
    };
}

VideoFrame videoAt(std::chrono::milliseconds timestamp) {
    return VideoFrame{.timestamp = creator::core::TimestampNs{timestamp},
                      .width = 16,
                      .height = 16,
                      .pixelFormat = creator::media::PixelFormat::Bgra8};
}

AudioBlock audioAt(std::chrono::milliseconds timestamp) {
    auto samples = std::shared_ptr<float[]>(new float[480 * 2]{});
    return AudioBlock{.timestamp = creator::core::TimestampNs{timestamp},
                      .sampleRate = 48'000,
                      .channels = 2,
                      .frameCount = 480,
                      .samples = std::move(samples)};
}

TEST(MultiTrackRecordingServiceTest, RoutesSourcesAndAggregatesAConcurrentStop) {
    const auto screen = SourceId::create("screen-1").value();
    const auto microphone = SourceId::create("microphone-1").value();
    auto screenTrack = buildTrack(screen, TrackRole::Screen);
    auto microphoneTrack = buildTrack(microphone, TrackRole::Microphone);
    MultiTrackRecordingService service;
    ASSERT_TRUE(service.addTrack(std::move(screenTrack.recorder)).hasValue());
    ASSERT_TRUE(service.addTrack(std::move(microphoneTrack.recorder)).hasValue());
    ASSERT_TRUE(service.start().hasValue());
    ASSERT_TRUE(service.accept(screen, videoAt(std::chrono::milliseconds{0})).hasValue());
    ASSERT_TRUE(service.accept(microphone, audioAt(std::chrono::milliseconds{0})).hasValue());
    auto firstPromise =
        std::make_shared<std::promise<Result<MultiTrackRecordingSummary>>>();
    auto secondPromise =
        std::make_shared<std::promise<Result<MultiTrackRecordingSummary>>>();
    auto first = firstPromise->get_future();
    auto second = secondPromise->get_future();

    service.stopAsync(creator::core::TimestampNs{std::chrono::milliseconds{20}},
                      [firstPromise](const auto& result) { firstPromise->set_value(result); });
    service.stopAsync(creator::core::TimestampNs{std::chrono::milliseconds{20}},
                      [secondPromise](const auto& result) { secondPromise->set_value(result); });

    const auto firstResult = first.get();
    const auto secondResult = second.get();
    ASSERT_TRUE(firstResult.hasValue()) << firstResult.error().message();
    ASSERT_TRUE(secondResult.hasValue()) << secondResult.error().message();
    ASSERT_EQ(firstResult.value().tracks.size(), 2u);
}

// A track whose own encoder/disk fails ends THAT track; the other sources keep
// recording and the take completes normally, carrying the failure per track.
TEST(MultiTrackRecordingServiceTest, OneTrackFailureStopsOnlyThatTrack) {
    const auto screen = SourceId::create("screen-1").value();
    const auto microphone = SourceId::create("microphone-1").value();
    auto screenTrack = buildTrack(screen, TrackRole::Screen);
    auto microphoneTrack = buildTrack(microphone, TrackRole::Microphone);
    auto* screenRecorder = screenTrack.recorder.get();
    auto* microphoneRecorder = microphoneTrack.recorder.get();
    screenTrack.encoder->acceptError =
        AppError{creator::core::ErrorCode::IoFailure, "screen encoder failed"};
    MultiTrackRecordingService service;
    ASSERT_TRUE(service.addTrack(std::move(screenTrack.recorder)).hasValue());
    ASSERT_TRUE(service.addTrack(std::move(microphoneTrack.recorder)).hasValue());
    auto promise = std::make_shared<std::promise<Result<MultiTrackRecordingSummary>>>();
    auto future = promise->get_future();
    service.observeCompletion(
        [promise](const auto& result) { promise->set_value(result); });
    ASSERT_TRUE(service.start().hasValue());
    ASSERT_TRUE(service.accept(microphone, audioAt(std::chrono::milliseconds{0})).hasValue());
    ASSERT_TRUE(service.accept(screen, videoAt(std::chrono::milliseconds{0})).hasValue());

    // The screen worker fails asynchronously; wait for its recorder to settle.
    for (int i = 0; i < 500 && screenRecorder->snapshot().state !=
                                   creator::recorder::TrackRecorderState::Stopped;
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    EXPECT_EQ(screenRecorder->snapshot().state,
              creator::recorder::TrackRecorderState::Stopped);
    EXPECT_EQ(service.snapshot().state, creator::app::MultiTrackRecordingState::Running);
    EXPECT_NE(microphoneRecorder->snapshot().state,
              creator::recorder::TrackRecorderState::Stopped);
    // The surviving track still records.
    ASSERT_TRUE(service.accept(microphone, audioAt(std::chrono::milliseconds{10})).hasValue());
    // The dead track refuses further data with its own failure, not a take-wide one.
    const auto rejected = service.accept(screen, videoAt(std::chrono::milliseconds{33}));
    EXPECT_FALSE(rejected.hasValue());
    const auto live = service.snapshot();
    ASSERT_EQ(live.tracks.size(), 2u);
    ASSERT_TRUE(live.tracks[0].failure.has_value());
    EXPECT_EQ(live.tracks[0].failure->message(), "screen encoder failed");
    EXPECT_FALSE(live.tracks[1].failure.has_value());
    EXPECT_FALSE(live.terminalError.has_value());

    service.stopAsync(creator::core::TimestampNs{std::chrono::milliseconds{40}});
    const auto result = future.get();
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    ASSERT_EQ(result.value().tracks.size(), 2u);
    ASSERT_TRUE(result.value().tracks[0].failure.has_value());
    EXPECT_EQ(result.value().tracks[0].failure->message(), "screen encoder failed");
    EXPECT_FALSE(result.value().tracks[1].failure.has_value());
}

// When every track has died there is nothing left to record: the take ends
// with the first failure as its terminal error.
TEST(MultiTrackRecordingServiceTest, LastSurvivingTrackFailureEndsTheTake) {
    const auto screen = SourceId::create("screen-1").value();
    auto screenTrack = buildTrack(screen, TrackRole::Screen);
    screenTrack.encoder->acceptError =
        AppError{creator::core::ErrorCode::IoFailure, "screen encoder failed"};
    MultiTrackRecordingService service;
    ASSERT_TRUE(service.addTrack(std::move(screenTrack.recorder)).hasValue());
    auto promise = std::make_shared<std::promise<Result<MultiTrackRecordingSummary>>>();
    auto future = promise->get_future();
    service.observeCompletion(
        [promise](const auto& result) { promise->set_value(result); });
    ASSERT_TRUE(service.start().hasValue());
    ASSERT_TRUE(service.accept(screen, videoAt(std::chrono::milliseconds{0})).hasValue());

    const auto result = future.get();
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().message(), "screen encoder failed");
    EXPECT_EQ(service.snapshot().state, creator::app::MultiTrackRecordingState::Stopped);
}

// A source that stops delivering (camera unplugged, audio endpoint changed)
// is a dropout: nothing is stopped, the track stays open for the source to
// resume, and the dropout is visible so the UI and log can report it.
TEST(MultiTrackRecordingServiceTest, SourceDropoutKeepsEveryTrackRecordingAndIsVisible) {
    const auto screen = SourceId::create("screen-1").value();
    const auto microphone = SourceId::create("microphone-1").value();
    auto screenTrack = buildTrack(screen, TrackRole::Screen);
    auto microphoneTrack = buildTrack(microphone, TrackRole::Microphone);
    MultiTrackRecordingService service;
    ASSERT_TRUE(service.addTrack(std::move(screenTrack.recorder)).hasValue());
    ASSERT_TRUE(service.addTrack(std::move(microphoneTrack.recorder)).hasValue());
    ASSERT_TRUE(service.start().hasValue());
    ASSERT_TRUE(service.accept(microphone, audioAt(std::chrono::milliseconds{0})).hasValue());

    service.noteSourceDropout(microphone,
                              {creator::core::ErrorCode::IoFailure, "endpoint invalidated"},
                              creator::core::TimestampNs{std::chrono::milliseconds{10}});

    auto snapshot = service.snapshot();
    EXPECT_EQ(snapshot.state, creator::app::MultiTrackRecordingState::Running);
    EXPECT_FALSE(snapshot.terminalError.has_value());
    ASSERT_EQ(snapshot.dropouts.size(), 1u);
    EXPECT_EQ(snapshot.dropouts[0].sourceId, microphone);
    EXPECT_EQ(snapshot.dropouts[0].error.message(), "endpoint invalidated");
    EXPECT_EQ(snapshot.dropouts[0].at, creator::core::TimestampNs{std::chrono::milliseconds{10}});
    // The source came back: its track still accepts.
    ASSERT_TRUE(service.accept(microphone, audioAt(std::chrono::milliseconds{500})).hasValue());
    ASSERT_TRUE(service.accept(screen, videoAt(std::chrono::milliseconds{500})).hasValue());

    auto promise = std::make_shared<std::promise<Result<MultiTrackRecordingSummary>>>();
    auto future = promise->get_future();
    service.stopAsync(creator::core::TimestampNs{std::chrono::milliseconds{600}},
                      [promise](const auto& result) { promise->set_value(result); });
    const auto result = future.get();
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    ASSERT_EQ(result.value().dropouts.size(), 1u);
    EXPECT_EQ(result.value().dropouts[0].error.message(), "endpoint invalidated");
}

TEST(MultiTrackRecordingServiceTest, RejectsDuplicateSourceTrack) {
    const auto source = SourceId::create("screen-1").value();
    auto first = buildTrack(source, TrackRole::Screen);
    auto duplicate = buildTrack(source, TrackRole::CompositePreview);
    MultiTrackRecordingService service;
    ASSERT_TRUE(service.addTrack(std::move(first.recorder)).hasValue());

    const auto result = service.addTrack(std::move(duplicate.recorder));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), creator::core::ErrorCode::AlreadyExists);
}

TEST(MultiTrackRecordingServiceTest, StartFailureCompletesObserverAndStopsService) {
    const auto source = SourceId::create("screen-1").value();
    auto invalid = buildTrack(source, TrackRole::Screen, 0);
    MultiTrackRecordingService service;
    ASSERT_TRUE(service.addTrack(std::move(invalid.recorder)).hasValue());
    auto promise = std::make_shared<std::promise<Result<MultiTrackRecordingSummary>>>();
    auto future = promise->get_future();
    service.observeCompletion(
        [promise](const auto& result) { promise->set_value(result); });

    const auto started = service.start();
    const auto completed = future.get();

    ASSERT_FALSE(started.hasValue());
    ASSERT_FALSE(completed.hasValue());
    EXPECT_EQ(service.snapshot().state, creator::app::MultiTrackRecordingState::Stopped);
}

// fail() is reserved for take-wide faults (clock/sync/planning). It still
// ends every track and completes exactly once.
TEST(MultiTrackRecordingServiceTest, TakeWideFailureStopsEveryTrackAndCompletesOnce) {
    const auto screen = SourceId::create("screen-1").value();
    const auto microphone = SourceId::create("microphone-1").value();
    auto screenTrack = buildTrack(screen, TrackRole::Screen);
    auto microphoneTrack = buildTrack(microphone, TrackRole::Microphone);
    MultiTrackRecordingService service;
    ASSERT_TRUE(service.addTrack(std::move(screenTrack.recorder)).hasValue());
    ASSERT_TRUE(service.addTrack(std::move(microphoneTrack.recorder)).hasValue());
    ASSERT_TRUE(service.start().hasValue());
    auto promise = std::make_shared<std::promise<Result<MultiTrackRecordingSummary>>>();
    auto future = promise->get_future();
    service.observeCompletion(
        [promise](const auto& result) { promise->set_value(result); });

    service.fail({creator::core::ErrorCode::NotFound, "camera disconnected"},
                 creator::core::TimestampNs{std::chrono::seconds{1}});
    service.fail({creator::core::ErrorCode::IoFailure, "late duplicate"},
                 creator::core::TimestampNs{std::chrono::seconds{2}});
    const auto completed = future.get();

    ASSERT_FALSE(completed.hasValue());
    EXPECT_EQ(completed.error().message(), "camera disconnected");
    const auto snapshot = service.snapshot();
    EXPECT_EQ(snapshot.state, creator::app::MultiTrackRecordingState::Stopped);
    ASSERT_TRUE(snapshot.terminalError.has_value());
    EXPECT_EQ(snapshot.terminalError->message(), "camera disconnected");
}

}  // namespace
