#include "app/EditorController.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/MediaAsset.h"
#include "domain/SplitClipCommand.h"
#include "domain/Timeline.h"
#include "domain/TimelineRevision.h"
#include "edit_engine/EditEngineTypes.h"
#include "edit_engine/IEditEngine.h"
#include "fakes/FakeEditEngine.h"

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QSignalSpy>
#include <QThread>
#include <QVariantList>

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using creator::app::EditorController;
using creator::app::MediaBinModel;
using creator::app::TimelineTrackModel;
using creator::core::DurationNs;
using creator::core::FrameRate;
using creator::core::TimestampNs;
using creator::domain::AssetAvailability;
using creator::domain::AssetId;
using creator::domain::AudioAssetMetadata;
using creator::domain::Clip;
using creator::domain::ClipId;
using creator::domain::CommandId;
using creator::domain::MediaAsset;
using creator::domain::MediaKind;
using creator::domain::SplitClipCommand;
using creator::domain::TimeRange;
using creator::domain::Timeline;
using creator::domain::TimelineId;
using creator::domain::TimelineRevision;
using creator::domain::Track;
using creator::domain::TrackId;
using creator::domain::TrackKind;
using creator::domain::VideoAssetMetadata;
using creator::edit_engine::IEditEngine;
using creator::edit_engine::IRenderJob;
using creator::edit_engine::PreviewFrame;
using creator::edit_engine::RenderRequest;
using creator::edit_engine::TimelineChangeSet;
using creator::edit_engine::TimelineSnapshot;
using creator::fakes::FakeEditEngine;
using creator::fakes::FakeEditOperation;

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

MediaAsset asset(std::string id = "screen") {
    const std::string path = "media/" + id + ".mp4";
    return MediaAsset::create(
               AssetId::create(std::move(id)).value(), MediaKind::Video,
               path, DurationNs{5'000'000'000},
               VideoAssetMetadata{1920, 1080, FrameRate::create(60, 1).value()},
               std::nullopt, 42'000, "fingerprint",
               AssetAvailability::Available)
        .value();
}

MediaAsset audioAsset(std::string id) {
    const std::string path = "media/" + id + ".wav";
    return MediaAsset::create(
               AssetId::create(std::move(id)).value(), MediaKind::Audio, path,
               DurationNs{5'000'000'000}, std::nullopt,
               AudioAssetMetadata{48'000, 2}, 21'000, "audio-fingerprint",
               AssetAvailability::Available)
        .value();
}

TimelineSnapshot snapshot(std::int64_t revision, std::string name,
                          std::string trackName = "Screen") {
    auto timeline = Timeline::create(TimelineId::create("main").value(),
                                     std::move(name),
                                     FrameRate::create(60, 1).value())
                        .value();
    EXPECT_TRUE(timeline.addTrack(
                            Track::create(TrackId::create("v1").value(),
                                          TrackKind::Video,
                                          std::move(trackName), true, false)
                                .value())
                    .hasValue());
    return TimelineSnapshot{std::move(timeline),
                            TimelineRevision::create(revision).value()};
}

TEST(EditorControllerTest, PublishesModelsBeforeAsynchronousEngineLoadCompletes) {
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    QSignalSpy mediaReset{controller.mediaBinModel(),
                          &QAbstractItemModel::modelReset};
    QSignalSpy tracksReset{controller.timelineTrackModel(),
                           &QAbstractItemModel::modelReset};

    controller.openSession({asset()}, snapshot(1, "강의", "화면"));

    EXPECT_EQ(controller.mediaBinModel()->rowCount(), 1);
    EXPECT_EQ(controller.timelineTrackModel()->rowCount(), 1);
    EXPECT_EQ(mediaReset.count(), 1);
    EXPECT_EQ(tracksReset.count(), 1);
    EXPECT_EQ(controller.timelineTrackModel()
                  ->data(controller.timelineTrackModel()->index(0, 0),
                         TimelineTrackModel::NameRole)
                  .toString(),
              QString::fromUtf8("화면"));
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() == 2U;
    }));
    ASSERT_EQ(fake->calls().size(), 2U);
    EXPECT_EQ(fake->calls()[0].operation, FakeEditOperation::Load);
    EXPECT_EQ(fake->calls()[1].operation, FakeEditOperation::RequestFrame);
    EXPECT_FALSE(controller.previewStale());
    EXPECT_EQ(controller.thread(), QThread::currentThread());
}

TEST(EditorControllerTest, SerializesPlaybackAndIgnoresStaleSessionCallback) {
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    fake->failNext(FakeEditOperation::Load,
                   creator::core::AppError{creator::core::ErrorCode::IoFailure,
                                           "old session failure"});
    EditorController controller{std::move(engine)};
    controller.openSession({asset("old")}, snapshot(1, "Old"));
    controller.openSession({asset("new")}, snapshot(2, "New"));
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() == 3U;
    }));

    EXPECT_TRUE(controller.statusMessage().isEmpty());
    EXPECT_FALSE(controller.previewStale());
    ASSERT_EQ(fake->calls().size(), 3U);
    EXPECT_EQ(fake->calls()[0].revision, 1);
    EXPECT_EQ(fake->calls()[1].revision, 2);
    EXPECT_EQ(fake->calls()[2].operation, FakeEditOperation::RequestFrame);

    controller.play();
    controller.seek(250);
    controller.pause();
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() == 7U;
    }));
    ASSERT_EQ(fake->calls().size(), 7U);
    EXPECT_EQ(fake->calls()[3].operation, FakeEditOperation::Play);
    EXPECT_EQ(fake->calls()[4].operation, FakeEditOperation::Seek);
    EXPECT_EQ(fake->calls()[5].operation, FakeEditOperation::Pause);
    EXPECT_EQ(fake->calls()[6].operation, FakeEditOperation::RequestFrame);
    EXPECT_FALSE(controller.playing());
    EXPECT_EQ(controller.playheadNs(), 250);
}

TEST(EditorControllerTest, PublishesCommittedTimelineBeforeEngineUpdate) {
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    controller.openSession({asset()}, snapshot(3, "Before", "Original"));
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() == 2U;
    }));
    auto change = TimelineChangeSet::create(
                      TimelineRevision::create(3).value(),
                      snapshot(4, "After", "Edited"),
                      {TrackId::create("v1").value()}, false)
                      .value();

    controller.commitTimeline(change);

    EXPECT_EQ(controller.timelineTrackModel()
                  ->data(controller.timelineTrackModel()->index(0, 0),
                         TimelineTrackModel::NameRole)
                  .toString(),
              QStringLiteral("Edited"));
    EXPECT_EQ(controller.timelineRevision(), 4);
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() == 4U;
    }));
    ASSERT_EQ(fake->calls().size(), 4U);
    EXPECT_EQ(fake->calls()[2].operation, FakeEditOperation::Update);
    EXPECT_EQ(fake->calls()[3].operation, FakeEditOperation::RequestFrame);
}

TEST(EditorControllerTest, FailedUpdateKeepsDurableStateAndReloadsPreview) {
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    controller.openSession({asset()}, snapshot(8, "Before", "Original"));
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() == 2U;
    }));
    fake->failNext(FakeEditOperation::Update,
                   creator::core::AppError{creator::core::ErrorCode::IoFailure,
                                           "preview graph update failed"});
    QSignalSpy staleSpy{&controller, &EditorController::previewStaleChanged};
    auto change = TimelineChangeSet::create(
                      TimelineRevision::create(8).value(),
                      snapshot(9, "Durable", "Committed"),
                      {TrackId::create("v1").value()}, false)
                      .value();

    controller.commitTimeline(change);
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() >= 5U;
    }));

    EXPECT_EQ(controller.timelineRevision(), 9);
    EXPECT_EQ(controller.timelineTrackModel()
                  ->data(controller.timelineTrackModel()->index(0, 0),
                         TimelineTrackModel::NameRole)
                  .toString(),
              QStringLiteral("Committed"));
    ASSERT_EQ(fake->calls().size(), 5U);
    EXPECT_EQ(fake->calls()[2].operation, FakeEditOperation::Update);
    EXPECT_EQ(fake->calls()[3].operation, FakeEditOperation::Load);
    EXPECT_EQ(fake->calls()[3].revision, 9);
    EXPECT_EQ(fake->calls()[4].operation, FakeEditOperation::RequestFrame);
    EXPECT_GE(staleSpy.count(), 2);
    EXPECT_FALSE(controller.previewStale());
    EXPECT_EQ(controller.statusMessage(),
              QStringLiteral("preview graph update failed"));
}

TEST(EditorControllerTest, RecoveryReloadRunsBeforePlaybackQueuedAfterFailedUpdate) {
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    controller.openSession({asset()}, snapshot(10, "Before"));
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() == 2U;
    }));
    fake->failNext(FakeEditOperation::Update,
                   creator::core::AppError{creator::core::ErrorCode::IoFailure,
                                           "force ordered recovery"});
    auto change = TimelineChangeSet::create(
                      TimelineRevision::create(10).value(),
                      snapshot(11, "After"),
                      {TrackId::create("v1").value()}, false)
                      .value();

    controller.commitTimeline(change);
    controller.play();
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && controller.playing() &&
               fake->calls().size() >= 5U;
    }));

    ASSERT_GE(fake->calls().size(), 5U);
    EXPECT_EQ(fake->calls()[0].operation, FakeEditOperation::Load);
    EXPECT_EQ(fake->calls()[1].operation, FakeEditOperation::RequestFrame);
    EXPECT_EQ(fake->calls()[2].operation, FakeEditOperation::Update);
    EXPECT_EQ(fake->calls()[3].operation, FakeEditOperation::Load);
    EXPECT_EQ(fake->calls()[4].operation, FakeEditOperation::Play);
    EXPECT_TRUE(controller.playing());
    controller.pause();
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && !controller.playing();
    }));
}

TEST(EditorControllerAcceptanceTest,
     OpensMultitrackTakeSeeksCommitsSplitAndRecoversPreviewGraph) {
    const MediaAsset screen = asset("screen");
    const MediaAsset camera = asset("camera");
    const MediaAsset microphone = audioAsset("microphone");
    auto timeline = Timeline::create(TimelineId::create("take-1").value(),
                                     "Recorded take",
                                     FrameRate::create(60, 1).value())
                        .value();
    const TrackId screenTrack = TrackId::create("screen-track").value();
    const TrackId cameraTrack = TrackId::create("camera-track").value();
    const TrackId microphoneTrack = TrackId::create("microphone-track").value();
    ASSERT_TRUE(timeline.addTrack(Track::create(screenTrack, TrackKind::Video,
                                                "Screen", true, false)
                                      .value())
                    .hasValue());
    ASSERT_TRUE(timeline.addTrack(Track::create(cameraTrack, TrackKind::Video,
                                                "Camera", true, false)
                                      .value())
                    .hasValue());
    ASSERT_TRUE(timeline.addTrack(Track::create(microphoneTrack, TrackKind::Audio,
                                                "Microphone", true, false)
                                      .value())
                    .hasValue());
    const auto range = TimeRange::create(TimestampNs{}, DurationNs{4'000'000'000})
                           .value();
    ASSERT_TRUE(timeline.insertClip(
                            screenTrack,
                            Clip::createAsset(ClipId::create("screen-clip").value(),
                                              screen, range, range, true,
                                              std::nullopt, std::nullopt)
                                .value())
                    .hasValue());
    ASSERT_TRUE(timeline.insertClip(
                            cameraTrack,
                            Clip::createAsset(ClipId::create("camera-clip").value(),
                                              camera, range, range, true,
                                              std::nullopt, std::nullopt)
                                .value())
                    .hasValue());
    ASSERT_TRUE(timeline.insertClip(
                            microphoneTrack,
                            Clip::createAsset(
                                ClipId::create("microphone-clip").value(), microphone,
                                range, range, true, std::nullopt, std::nullopt)
                                .value())
                    .hasValue());

    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    controller.openSession(
        {screen, camera, microphone},
        TimelineSnapshot{timeline, TimelineRevision::create(1).value()});
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() == 2U;
    }));
    EXPECT_EQ(controller.mediaBinModel()->rowCount(), 3);
    EXPECT_EQ(controller.timelineTrackModel()->rowCount(), 3);

    controller.seek(2'000'000'000);
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() == 4U;
    }));
    EXPECT_EQ(controller.playheadNs(), 2'000'000'000);

    SplitClipCommand split{
        CommandId::create("split-screen").value(), screenTrack,
        ClipId::create("screen-clip").value(),
        ClipId::create("screen-clip-right").value(),
        TimestampNs{DurationNs{2'000'000'000}}};
    ASSERT_TRUE(split.execute(timeline).hasValue());
    auto change = TimelineChangeSet::create(
                      TimelineRevision::create(1).value(),
                      TimelineSnapshot{timeline,
                                       TimelineRevision::create(2).value()},
                      {screenTrack}, false)
                      .value();
    fake->failNext(FakeEditOperation::Update,
                   creator::core::AppError{creator::core::ErrorCode::IoFailure,
                                           "rebuild acceptance failure"});

    controller.commitTimeline(change);

    const QVariantList committedClips =
        controller.timelineTrackModel()
            ->data(controller.timelineTrackModel()->index(0, 0),
                   TimelineTrackModel::ClipsRole)
            .toList();
    EXPECT_EQ(committedClips.size(), 2);
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() >= 7U;
    }));
    EXPECT_EQ(controller.timelineRevision(), 2);
    EXPECT_FALSE(controller.previewStale());
    ASSERT_EQ(fake->calls().size(), 7U);
    EXPECT_EQ(fake->calls()[0].operation, FakeEditOperation::Load);
    EXPECT_EQ(fake->calls()[1].operation, FakeEditOperation::RequestFrame);
    EXPECT_EQ(fake->calls()[2].operation, FakeEditOperation::Seek);
    EXPECT_EQ(fake->calls()[3].operation, FakeEditOperation::RequestFrame);
    EXPECT_EQ(fake->calls()[4].operation, FakeEditOperation::Update);
    EXPECT_EQ(fake->calls()[5].operation, FakeEditOperation::Load);
    EXPECT_EQ(fake->calls()[6].operation, FakeEditOperation::RequestFrame);
}

struct DrainState final {
    std::atomic<int> calls{0};
    std::thread::id uiThread;
    std::atomic<bool> loadRanOffUiThread{false};
};

struct PreviewThreadState final {
    std::thread::id uiThread;
    std::atomic<bool> frameRanOffUiThread{false};
    std::atomic<std::int64_t> requestedPosition{-1};
    std::atomic<int> frameCalls{0};
    std::atomic<int> activeFrameCalls{0};
    std::atomic<int> maximumActiveFrameCalls{0};
    std::atomic<int> blockCall{0};
    std::atomic<bool> blockedCallEntered{false};
    std::mutex blockMutex;
    std::condition_variable blockCondition;
    bool releaseBlockedCall{false};
};

class PreviewThreadEngine final : public IEditEngine {
public:
    explicit PreviewThreadEngine(std::shared_ptr<PreviewThreadState> state)
        : state_(std::move(state)) {}

    creator::core::Result<void> load(const TimelineSnapshot& value) override {
        revision_ = value.revision;
        return creator::core::ok();
    }
    creator::core::Result<void> update(const TimelineChangeSet& change) override {
        revision_ = change.target().revision;
        return creator::core::ok();
    }
    creator::core::Result<void> play() override { return creator::core::ok(); }
    creator::core::Result<void> pause() override { return creator::core::ok(); }
    creator::core::Result<void> seek(TimestampNs) override {
        return creator::core::ok();
    }
    creator::core::Result<PreviewFrame> requestFrame(
        TimestampNs position) override {
        const int call = ++state_->frameCalls;
        const int active = ++state_->activeFrameCalls;
        int previousMaximum = state_->maximumActiveFrameCalls.load();
        while (active > previousMaximum &&
               !state_->maximumActiveFrameCalls.compare_exchange_weak(
                   previousMaximum, active)) {
        }
        state_->frameRanOffUiThread =
            std::this_thread::get_id() != state_->uiThread;
        state_->requestedPosition = position.time_since_epoch().count();
        if (call == state_->blockCall.load()) {
            std::unique_lock lock{state_->blockMutex};
            state_->blockedCallEntered = true;
            state_->blockCondition.notify_all();
            state_->blockCondition.wait(
                lock, [&] { return state_->releaseBlockedCall; });
        }
        auto pixels = std::make_shared<std::vector<std::uint8_t>>(
            std::initializer_list<std::uint8_t>{0x10, 0x20, 0x30, 0xff,
                                                0x40, 0x50, 0x60, 0xff});
        (*pixels)[2] = static_cast<std::uint8_t>(revision_.value());
        --state_->activeFrameCalls;
        creator::media::VideoFrame frame{
            .timestamp = position,
            .width = 2,
            .height = 1,
            .visibleRect = {.x = 0, .y = 0, .width = 2, .height = 1},
            .contentWidth = 2,
            .contentHeight = 1,
            .contentScale = 1.0,
            .pointPixelScale = 1.0,
            .pixelFormat = creator::media::PixelFormat::Bgra8,
            .colorSpace = creator::media::ColorSpace::Rec709Sdr,
            .platformHandle =
                std::shared_ptr<void>{pixels, pixels->data()}};
        return PreviewFrame::create(position, revision_, std::move(frame));
    }
    creator::core::Result<std::unique_ptr<IRenderJob>> render(
        const RenderRequest&) override {
        return creator::core::AppError{creator::core::ErrorCode::InvalidState,
                                       "not used"};
    }

private:
    std::shared_ptr<PreviewThreadState> state_;
    TimelineRevision revision_{TimelineRevision::create(0).value()};
};

class DrainCountingEngine final : public IEditEngine {
public:
    explicit DrainCountingEngine(std::shared_ptr<DrainState> state)
        : state_(std::move(state)) {}

    creator::core::Result<void> load(const TimelineSnapshot&) override {
        ++state_->calls;
        state_->loadRanOffUiThread =
            std::this_thread::get_id() != state_->uiThread;
        return creator::core::ok();
    }
    creator::core::Result<void> update(const TimelineChangeSet&) override {
        ++state_->calls;
        return creator::core::ok();
    }
    creator::core::Result<void> play() override {
        ++state_->calls;
        return creator::core::ok();
    }
    creator::core::Result<void> pause() override {
        ++state_->calls;
        return creator::core::ok();
    }
    creator::core::Result<void> seek(TimestampNs) override {
        ++state_->calls;
        return creator::core::ok();
    }
    creator::core::Result<PreviewFrame> requestFrame(TimestampNs) override {
        return creator::core::AppError{creator::core::ErrorCode::InvalidState,
                                       "not used"};
    }
    creator::core::Result<std::unique_ptr<IRenderJob>> render(
        const RenderRequest&) override {
        return creator::core::AppError{creator::core::ErrorCode::InvalidState,
                                       "not used"};
    }

private:
    std::shared_ptr<DrainState> state_;
};

TEST(EditorControllerTest, RunsEngineLoadOffTheUiThread) {
    auto state = std::make_shared<DrainState>();
    state->uiThread = std::this_thread::get_id();
    EditorController controller{std::make_unique<DrainCountingEngine>(state)};
    controller.openSession({asset()}, snapshot(1, "Threaded"));
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    EXPECT_TRUE(state->loadRanOffUiThread.load());
}

TEST(EditorControllerTest, RequestsDetachedPreviewFrameOffTheUiThread) {
    auto state = std::make_shared<PreviewThreadState>();
    state->uiThread = std::this_thread::get_id();
    EditorController controller{
        std::make_unique<PreviewThreadEngine>(state)};

    controller.openSession({asset()}, snapshot(7, "Preview worker"));

    ASSERT_TRUE(waitUntil([&] { return !controller.previewImage().isNull(); }));
    EXPECT_TRUE(state->frameRanOffUiThread.load());
    EXPECT_EQ(state->requestedPosition.load(), 0);
    ASSERT_EQ(controller.previewImage().size(), QSize(2, 1));
    EXPECT_EQ(controller.previewImage().pixelColor(0, 0),
              QColor(0x07, 0x20, 0x10));
}

TEST(EditorControllerTest, SeekRequestsTheExactPreviewFrame) {
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    controller.openSession({asset()}, snapshot(12, "Exact seek"));
    ASSERT_TRUE(waitUntil([&] { return fake->calls().size() == 2U; }));

    constexpr qlonglong kPosition = 1'234'567'890;
    controller.seek(kPosition);

    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() == 4U;
    }));
    EXPECT_EQ(fake->calls()[2].operation, FakeEditOperation::Seek);
    EXPECT_EQ(fake->calls()[3].operation, FakeEditOperation::RequestFrame);
    ASSERT_TRUE(fake->calls()[3].position.has_value());
    EXPECT_EQ(fake->calls()[3].position->time_since_epoch().count(), kPosition);
    EXPECT_EQ(controller.playheadNs(), kPosition);
}

TEST(EditorControllerTest, PlaybackAdvancesAtTimelineRateAndPauseStopsIt) {
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    controller.openSession({asset()}, snapshot(13, "Playback clock"));
    ASSERT_TRUE(waitUntil([&] { return fake->calls().size() == 2U; }));

    controller.play();
    ASSERT_TRUE(waitUntil([&] {
        return controller.playing() && controller.playheadNs() > 0 &&
               fake->calls().size() >= 4U;
    }, 1000));
    const qlonglong advancedPosition = controller.playheadNs();
    const auto rate = FrameRate::create(60, 1).value();
    const auto frameIndex = creator::core::timestampToFrame(
        TimestampNs{DurationNs{advancedPosition}}, rate);
    EXPECT_EQ(creator::core::frameToTimestamp(frameIndex, rate)
                  .time_since_epoch()
                  .count(),
              advancedPosition);

    controller.pause();
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && !controller.playing();
    }));
    const qlonglong pausedPosition = controller.playheadNs();
    QElapsedTimer pauseObservation;
    pauseObservation.start();
    while (pauseObservation.elapsed() < 80) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    EXPECT_GE(pausedPosition, advancedPosition);
    EXPECT_EQ(controller.playheadNs(), pausedPosition);
}

TEST(EditorControllerTest, PlaybackKeepsOnlyOneFrameRequestInFlight) {
    auto state = std::make_shared<PreviewThreadState>();
    state->uiThread = std::this_thread::get_id();
    EditorController controller{
        std::make_unique<PreviewThreadEngine>(state)};
    controller.openSession({asset()}, snapshot(14, "Backpressure"));
    ASSERT_TRUE(waitUntil([&] {
        return state->frameCalls.load() == 1 && controller.hasPreviewFrame();
    }));
    state->blockCall = 2;

    controller.play();
    const bool blocked = waitUntil(
        [&] { return state->blockedCallEntered.load(); }, 1000);
    EXPECT_TRUE(blocked);
    QElapsedTimer observation;
    observation.start();
    while (observation.elapsed() < 100) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    EXPECT_EQ(state->frameCalls.load(), 2);
    EXPECT_EQ(state->maximumActiveFrameCalls.load(), 1);

    controller.pause();
    {
        std::lock_guard lock{state->blockMutex};
        state->releaseBlockedCall = true;
    }
    state->blockCondition.notify_all();
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && !controller.playing();
    }));
}

TEST(EditorControllerTest, DropsLateFrameFromReplacedSession) {
    auto state = std::make_shared<PreviewThreadState>();
    state->uiThread = std::this_thread::get_id();
    state->blockCall = 1;
    EditorController controller{
        std::make_unique<PreviewThreadEngine>(state)};
    QSignalSpy frameSpy{&controller, &EditorController::previewImageChanged};
    controller.openSession({asset("old")}, snapshot(1, "Old preview"));
    const bool blocked = waitUntil(
        [&] { return state->blockedCallEntered.load(); }, 1000);
    EXPECT_TRUE(blocked);

    controller.openSession({asset("new")}, snapshot(2, "New preview"));
    {
        std::lock_guard lock{state->blockMutex};
        state->releaseBlockedCall = true;
    }
    state->blockCondition.notify_all();

    ASSERT_TRUE(waitUntil([&] {
        return state->frameCalls.load() == 2 && controller.hasPreviewFrame();
    }));
    EXPECT_EQ(frameSpy.count(), 1);
    EXPECT_EQ(controller.previewImage().pixelColor(0, 0),
              QColor(0x02, 0x20, 0x10));
    EXPECT_EQ(controller.timelineRevision(), 2);
}

TEST(EditorControllerTest, DropsLateFrameFromReplacedTimelineRevision) {
    auto state = std::make_shared<PreviewThreadState>();
    state->uiThread = std::this_thread::get_id();
    EditorController controller{
        std::make_unique<PreviewThreadEngine>(state)};
    controller.openSession({asset()}, snapshot(20, "Before revision"));
    ASSERT_TRUE(waitUntil([&] {
        return state->frameCalls.load() == 1 && controller.hasPreviewFrame();
    }));
    QSignalSpy frameSpy{&controller, &EditorController::previewImageChanged};
    state->blockCall = 2;
    controller.seek(100);
    const bool blocked = waitUntil(
        [&] { return state->blockedCallEntered.load(); }, 1000);
    EXPECT_TRUE(blocked);

    auto change = TimelineChangeSet::create(
                      TimelineRevision::create(20).value(),
                      snapshot(21, "After revision"),
                      {TrackId::create("v1").value()}, false)
                      .value();
    controller.commitTimeline(std::move(change));
    {
        std::lock_guard lock{state->blockMutex};
        state->releaseBlockedCall = true;
    }
    state->blockCondition.notify_all();

    ASSERT_TRUE(waitUntil([&] {
        return state->frameCalls.load() == 3 && !controller.previewStale();
    }));
    EXPECT_EQ(frameSpy.count(), 1);
    EXPECT_EQ(controller.previewImage().pixelColor(0, 0),
              QColor(0x15, 0x20, 0x10));
    EXPECT_TRUE(controller.statusMessage().isEmpty());
    EXPECT_EQ(controller.timelineRevision(), 21);
}

TEST(EditorControllerTest, FrameFailureMarksOnlyPreviewStale) {
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    controller.openSession({asset()}, snapshot(15, "Frame failure"));
    ASSERT_TRUE(waitUntil([&] { return fake->calls().size() == 2U; }));
    fake->failNext(FakeEditOperation::RequestFrame,
                   creator::core::AppError{creator::core::ErrorCode::IoFailure,
                                           "preview decode failed"});

    controller.seek(500'000'000);

    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && controller.previewStale();
    }));
    EXPECT_EQ(controller.timelineRevision(), 15);
    EXPECT_EQ(controller.playheadNs(), 500'000'000);
    EXPECT_EQ(controller.statusMessage(), QStringLiteral("preview decode failed"));
}

TEST(EditorControllerTest, DestructionDrainsEachQueuedEngineCommandExactlyOnce) {
    auto state = std::make_shared<DrainState>();
    state->uiThread = std::this_thread::get_id();
    {
        EditorController controller{
            std::make_unique<DrainCountingEngine>(state)};
        controller.openSession({asset()}, snapshot(1, "Drain"));
        controller.play();
        controller.seek(100);
        controller.pause();
    }
    EXPECT_EQ(state->calls.load(), 4);
}

}  // namespace
