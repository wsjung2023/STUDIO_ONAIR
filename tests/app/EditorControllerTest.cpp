#include "app/EditorController.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/MediaAsset.h"
#include "domain/Timeline.h"
#include "domain/TimelineRevision.h"
#include "edit_engine/EditEngineTypes.h"
#include "edit_engine/IEditEngine.h"
#include "fakes/FakeEditEngine.h"

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QThread>

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <memory>
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
using creator::domain::MediaAsset;
using creator::domain::MediaKind;
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
    return MediaAsset::create(
               AssetId::create(std::move(id)).value(), MediaKind::Video,
               "media/screen.mp4", DurationNs{5'000'000'000},
               VideoAssetMetadata{1920, 1080, FrameRate::create(60, 1).value()},
               std::nullopt, 42'000, "fingerprint",
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
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    ASSERT_EQ(fake->calls().size(), 1U);
    EXPECT_EQ(fake->calls().front().operation, FakeEditOperation::Load);
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
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));

    EXPECT_TRUE(controller.statusMessage().isEmpty());
    EXPECT_FALSE(controller.previewStale());
    ASSERT_EQ(fake->calls().size(), 2U);
    EXPECT_EQ(fake->calls()[0].revision, 1);
    EXPECT_EQ(fake->calls()[1].revision, 2);

    controller.play();
    controller.seek(250);
    controller.pause();
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    ASSERT_EQ(fake->calls().size(), 5U);
    EXPECT_EQ(fake->calls()[2].operation, FakeEditOperation::Play);
    EXPECT_EQ(fake->calls()[3].operation, FakeEditOperation::Seek);
    EXPECT_EQ(fake->calls()[4].operation, FakeEditOperation::Pause);
    EXPECT_FALSE(controller.playing());
    EXPECT_EQ(controller.playheadNs(), 250);
}

TEST(EditorControllerTest, PublishesCommittedTimelineBeforeEngineUpdate) {
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    controller.openSession({asset()}, snapshot(3, "Before", "Original"));
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
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
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    ASSERT_EQ(fake->calls().size(), 2U);
    EXPECT_EQ(fake->calls()[1].operation, FakeEditOperation::Update);
}

TEST(EditorControllerTest, FailedUpdateKeepsDurableStateAndReloadsPreview) {
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    controller.openSession({asset()}, snapshot(8, "Before", "Original"));
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
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
        return !controller.busy() && fake->calls().size() >= 3U;
    }));

    EXPECT_EQ(controller.timelineRevision(), 9);
    EXPECT_EQ(controller.timelineTrackModel()
                  ->data(controller.timelineTrackModel()->index(0, 0),
                         TimelineTrackModel::NameRole)
                  .toString(),
              QStringLiteral("Committed"));
    ASSERT_EQ(fake->calls().size(), 3U);
    EXPECT_EQ(fake->calls()[1].operation, FakeEditOperation::Update);
    EXPECT_EQ(fake->calls()[2].operation, FakeEditOperation::Load);
    EXPECT_EQ(fake->calls()[2].revision, 9);
    EXPECT_GE(staleSpy.count(), 2);
    EXPECT_FALSE(controller.previewStale());
    EXPECT_EQ(controller.statusMessage(),
              QStringLiteral("preview graph update failed"));
}

struct DrainState final {
    std::atomic<int> calls{0};
    std::thread::id uiThread;
    std::atomic<bool> loadRanOffUiThread{false};
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
