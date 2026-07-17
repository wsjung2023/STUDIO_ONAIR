#include "app/EditorSessionWorker.h"

#include "domain/MediaAsset.h"
#include "domain/Timeline.h"
#include "project_store/JsonProjectStore.h"
#include "project_store/ProjectPackageStore.h"
#include "project_store/SqliteTimelineStore.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <gtest/gtest.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace {

namespace fs = std::filesystem;

using creator::app::EditorSessionResultPtr;
using creator::app::EditorSessionWorker;
using creator::app::EditorEditKind;
using creator::app::EditorEditRequest;
using creator::app::TimelineStoreFactory;
using creator::core::DurationNs;
using creator::core::FrameRate;
using creator::core::TimestampNs;
using creator::domain::AssetAvailability;
using creator::domain::AssetId;
using creator::domain::AudioAssetMetadata;
using creator::domain::AudioEnvelope;
using creator::domain::Clip;
using creator::domain::ClipId;
using creator::domain::MediaAsset;
using creator::domain::MediaKind;
using creator::domain::TimeRange;
using creator::domain::Timeline;
using creator::domain::TimelineId;
using creator::domain::Track;
using creator::domain::TrackId;
using creator::domain::TrackKind;
using creator::domain::RgbaColor;
using creator::domain::TextAlignment;
using creator::domain::TitlePayload;
using creator::domain::VisualTransform;
using creator::domain::VideoAssetMetadata;
using creator::project_store::ProjectPackageStore;
using creator::project_store::JsonProjectStore;
using creator::project_store::SqliteTimelineStore;

MediaAsset videoAsset();
Timeline oneClipTimeline();

class FailingCommitStore final : public creator::project_store::ITimelineStore {
public:
    explicit FailingCommitStore(Timeline timeline)
        : timeline_(std::move(timeline)) {}

    creator::core::Result<void> putAsset(const MediaAsset&) override {
        return creator::core::ok();
    }
    creator::core::Result<MediaAsset> asset(const AssetId& id) override {
        if (id == videoAsset().id()) return videoAsset();
        return creator::core::AppError{creator::core::ErrorCode::NotFound,
                                       "asset was not found"};
    }
    creator::core::Result<std::vector<MediaAsset>> assets() override {
        return std::vector<MediaAsset>{videoAsset()};
    }
    creator::core::Result<void> createTimeline(const Timeline& timeline) override {
        timeline_ = timeline;
        return creator::core::ok();
    }
    creator::core::Result<creator::project_store::PersistedTimeline>
    loadPrimaryTimeline() override {
        return persisted();
    }
    creator::core::Result<creator::domain::EditHistory> loadEditHistory(
        std::size_t limit) override {
        return creator::domain::EditHistory{limit};
    }
    creator::core::Result<creator::project_store::PersistedEditSession>
    loadEditSession(std::size_t historyLimit) override {
        return creator::project_store::PersistedEditSession{
            .persisted = persisted(),
            .history = creator::domain::EditHistory{historyLimit},
        };
    }
    creator::core::Result<void> commitEdit(
        const creator::project_store::TimelineCommit&) override {
        ++commitCalls;
        return creator::core::AppError{creator::core::ErrorCode::IoFailure,
                                       "injected commit failure"};
    }
    creator::core::Result<void> markExplicitSave(
        const TimelineId&, std::int64_t, std::size_t) override {
        return creator::core::ok();
    }

    std::size_t commitCalls{0};

private:
    creator::project_store::PersistedTimeline persisted() const {
        return creator::project_store::PersistedTimeline{
            .timeline = timeline_,
            .revision = 0,
            .historyCount = 0,
            .historyCursor = 0,
            .cleanCursor = 0,
            .explicitSavedRevision = 0,
            .events = {},
        };
    }

    Timeline timeline_;
};

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

TimestampNs at(std::int64_t value) {
    return TimestampNs{DurationNs{value}};
}

MediaAsset videoAsset() {
    return MediaAsset::create(
               AssetId::create("asset-1").value(), MediaKind::Video,
               "media/screen.mp4", DurationNs{1'000},
               VideoAssetMetadata{.width = 1920,
                                  .height = 1080,
                                  .frameRate = FrameRate::create(60, 1).value()},
               AudioAssetMetadata{.sampleRate = 48'000, .channels = 2}, 100,
               "fingerprint", AssetAvailability::Available)
        .value();
}

Timeline oneClipTimeline() {
    auto timeline = Timeline::create(TimelineId::create("main").value(), "Main",
                                     FrameRate::create(60, 1).value())
                        .value();
    const auto trackId = TrackId::create("video-1").value();
    EXPECT_TRUE(timeline.addTrack(
                            Track::create(trackId, TrackKind::Video, "Video 1", true,
                                          false)
                                .value())
                    .hasValue());
    const auto full = TimeRange::create(at(0), DurationNs{1'000}).value();
    EXPECT_TRUE(timeline.insertClip(
                            trackId,
                            Clip::createAsset(ClipId::create("clip-1").value(),
                                              videoAsset(), full, full, true,
                                              std::nullopt, std::nullopt)
                                .value())
                    .hasValue());
    return timeline;
}

class TempPackage final {
public:
    TempPackage() {
        root_ = fs::temp_directory_path() /
                (fs::path{L"creator-studio-editor-영속-패키지-"} /
                 std::to_wstring(++nextId_));
        package_ = root_ / fs::path{L"프로젝트.cstudio"};
        std::error_code error;
        fs::remove_all(root_, error);
        fs::create_directories(root_);
        ProjectPackageStore store;
        auto created = store.create(package_, "Durable editor");
        EXPECT_TRUE(created.hasValue())
            << (created.hasValue() ? "" : created.error().message());
        if (created.hasValue()) manifest_ = created.value().package.manifest;
    }

    ~TempPackage() {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    [[nodiscard]] const fs::path& path() const noexcept { return package_; }

    void setCanvas(std::int32_t width, std::int32_t height) {
        ASSERT_TRUE(manifest_.has_value());
        manifest_->canvas.width = width;
        manifest_->canvas.height = height;
        JsonProjectStore store;
        auto saved = store.save(package_, *manifest_);
        ASSERT_TRUE(saved.hasValue())
            << (saved.hasValue() ? "" : saved.error().message());
    }

    void seedOneClipTimeline() const {
        auto storeResult = SqliteTimelineStore::open(
            package_ / manifest_->database, manifest_->projectId);
        ASSERT_TRUE(storeResult.hasValue()) << storeResult.error().message();
        auto store = std::move(storeResult).value();
        ASSERT_TRUE(store.putAsset(videoAsset()).hasValue());
        ASSERT_TRUE(store.createTimeline(oneClipTimeline()).hasValue());
    }

private:
    inline static std::uint64_t nextId_{0};
    fs::path root_;
    fs::path package_;
    std::optional<creator::domain::ProjectManifest> manifest_;
};

TEST(EditorSessionWorkerTest, OpensUnicodePackageAndPersistsDefaultTimeline) {
    TempPackage package;
    package.setCanvas(1080, 1920);
    QThread workerThread;
    auto* worker = new EditorSessionWorker;
    worker->moveToThread(&workerThread);
    QObject::connect(&workerThread, &QThread::finished, worker,
                     &QObject::deleteLater);

    std::mutex resultMutex;
    EditorSessionResultPtr result;
    quint64 receivedGeneration = 0;
    QThread* callbackThread = nullptr;
    QObject::connect(
        worker, &EditorSessionWorker::opened, worker,
        [&](quint64 generation, EditorSessionResultPtr value) {
            std::lock_guard lock{resultMutex};
            receivedGeneration = generation;
            callbackThread = QThread::currentThread();
            result = std::move(value);
        },
        Qt::DirectConnection);

    workerThread.start();
    QMetaObject::invokeMethod(
        worker,
        [worker, path = package.path()] { worker->openProject(7, path); },
        Qt::QueuedConnection);

    ASSERT_TRUE(waitUntil([&] {
        std::lock_guard lock{resultMutex};
        return static_cast<bool>(result);
    }));

    EditorSessionResultPtr captured;
    {
        std::lock_guard lock{resultMutex};
        captured = result;
    }
    ASSERT_NE(captured, nullptr);
    ASSERT_TRUE(captured->hasValue()) << captured->error().message();
    EXPECT_EQ(receivedGeneration, 7U);
    EXPECT_EQ(callbackThread, &workerThread);

    const auto& state = captured->value().state;
    EXPECT_EQ(state.snapshot.mediaRoot, package.path());
    EXPECT_EQ(state.snapshot.canvasWidth, 1080);
    EXPECT_EQ(state.snapshot.canvasHeight, 1920);
    EXPECT_EQ(state.snapshot.timeline.name(), "Main");
    EXPECT_EQ(state.snapshot.timeline.frameRate().numerator(), 60);
    EXPECT_EQ(state.snapshot.timeline.frameRate().denominator(), 1);
    ASSERT_EQ(state.snapshot.timeline.tracks().size(), 2U);
    EXPECT_EQ(state.snapshot.timeline.tracks()[0].id().value(), "video-1");
    EXPECT_EQ(state.snapshot.timeline.tracks()[0].kind(), TrackKind::Video);
    EXPECT_EQ(state.snapshot.timeline.tracks()[1].id().value(), "audio-1");
    EXPECT_EQ(state.snapshot.timeline.tracks()[1].kind(), TrackKind::Audio);
    EXPECT_EQ(state.snapshot.revision.value(), 0);
    EXPECT_TRUE(state.clean);
    EXPECT_FALSE(state.canUndo);
    EXPECT_FALSE(state.canRedo);
    EXPECT_EQ(state.historyCursor, 0U);

    workerThread.quit();
    ASSERT_TRUE(workerThread.wait(3000));

    ProjectPackageStore store;
    auto reopened = store.open(package.path());
    ASSERT_TRUE(reopened.hasValue()) << reopened.error().message();
}

TEST(EditorSessionWorkerTest, RoutesEveryEffectAndGeneratedEditDurably) {
    TempPackage package;
    package.seedOneClipTimeline();
    EditorSessionWorker worker;
    EditorSessionResultPtr openedResult;
    EditorSessionResultPtr editedResult;
    QObject::connect(
        &worker, &EditorSessionWorker::opened, &worker,
        [&](quint64, EditorSessionResultPtr value) {
            openedResult = std::move(value);
        },
        Qt::DirectConnection);
    QObject::connect(
        &worker, &EditorSessionWorker::edited, &worker,
        [&](quint64, quint64, EditorSessionResultPtr value) {
            editedResult = std::move(value);
        },
        Qt::DirectConnection);
    worker.openProject(20, package.path());
    ASSERT_NE(openedResult, nullptr);
    ASSERT_TRUE(openedResult->hasValue()) << openedResult->error().message();

    std::int64_t expectedRevision = 0;
    const auto run = [&](EditorEditRequest request, bool fullRebuild) {
        editedResult.reset();
        worker.edit(20, static_cast<quint64>(expectedRevision + 1),
                    std::move(request));
        ASSERT_NE(editedResult, nullptr);
        ASSERT_TRUE(editedResult->hasValue())
            << (editedResult->hasValue() ? ""
                                         : editedResult->error().message());
        ++expectedRevision;
        EXPECT_EQ(editedResult->value().state.snapshot.revision.value(),
                  expectedRevision);
        ASSERT_TRUE(editedResult->value().change.has_value());
        EXPECT_EQ(editedResult->value().change->requiresFullRebuild(),
                  fullRebuild);
    };
    const auto videoTrack = TrackId::create("video-1").value();
    const auto assetClip = ClipId::create("clip-1").value();
    const auto visual = VisualTransform::create(
                            0.1, 0.1, 0.5, 0.5, 1.0, 1.0, 5.0, 0.0, 0.0,
                            0.0, 0.0, 0.9, 3)
                            .value();
    run(EditorEditRequest{.kind = EditorEditKind::SetVisualTransform,
                          .trackId = videoTrack,
                          .clipId = assetClip,
                          .visualTransform = visual},
        false);
    EXPECT_EQ(editedResult->value()
                  .state.snapshot.timeline.clip(videoTrack, assetClip)
                  ->visualTransform(),
              visual);

    const auto audio = AudioEnvelope::create(-3.0, DurationNs{100},
                                              DurationNs{200}, DurationNs{1000})
                           .value();
    run(EditorEditRequest{.kind = EditorEditKind::SetAudioEnvelope,
                          .trackId = videoTrack,
                          .clipId = assetClip,
                          .audioEnvelope = audio},
        false);

    const auto title = TitlePayload::create(
                           "Hello", "Creator Sans", 0.5, 0.2,
                           RgbaColor::parse("#ffffffff").value(),
                           RgbaColor::parse("#00000000").value(),
                           TextAlignment::Center)
                           .value();
    run(EditorEditRequest{
            .kind = EditorEditKind::AddTitle,
            .range = TimeRange::create(at(100), DurationNs{400}).value(),
            .titlePayload = title,
        },
        true);
    const auto* titleTrack = editedResult->value().state.snapshot.timeline.track(
        TrackId::create("title-1").value());
    ASSERT_NE(titleTrack, nullptr);
    ASSERT_EQ(titleTrack->clips().size(), 1U);
    const auto titleClip = titleTrack->clips()[0].id();
    EXPECT_NE(titleClip.value(), "title-1");

    const auto editedTitle = TitlePayload::create(
                                 "Edited", "Creator Sans", 0.4, 0.3,
                                 RgbaColor::parse("#ffcc00ff").value(),
                                 RgbaColor::parse("#00000000").value(),
                                 TextAlignment::Left)
                                 .value();
    run(EditorEditRequest{.kind = EditorEditKind::EditTitle,
                          .trackId = TrackId::create("title-1").value(),
                          .clipId = titleClip,
                          .titlePayload = editedTitle},
        true);

    run(EditorEditRequest{
            .kind = EditorEditKind::AddCaptionCue,
            .range = TimeRange::create(at(0), DurationNs{1000}).value(),
            .captionCue = creator::app::CaptionCueDraft{
                .startOffset = DurationNs{100},
                .duration = DurationNs{300},
                .text = "First caption"},
        },
        true);
    const auto captionTrackId = TrackId::create("caption-1").value();
    const auto* captionTrack =
        editedResult->value().state.snapshot.timeline.track(captionTrackId);
    ASSERT_NE(captionTrack, nullptr);
    ASSERT_EQ(captionTrack->clips().size(), 1U);
    const auto captionClip = captionTrack->clips()[0].id();
    ASSERT_EQ(captionTrack->clips()[0].captionCues().size(), 1U);
    const auto cueId = captionTrack->clips()[0].captionCues()[0].id();
    EXPECT_FALSE(cueId.value().empty());

    run(EditorEditRequest{
            .kind = EditorEditKind::EditCaptionCue,
            .trackId = captionTrackId,
            .clipId = captionClip,
            .cueId = cueId,
            .captionCue = creator::app::CaptionCueDraft{
                .startOffset = DurationNs{200},
                .duration = DurationNs{250},
                .text = "Edited caption"},
        },
        true);
    run(EditorEditRequest{.kind = EditorEditKind::RemoveCaptionCue,
                          .trackId = captionTrackId,
                          .clipId = captionClip,
                          .cueId = cueId},
        true);
    run(EditorEditRequest{.kind = EditorEditKind::RemoveGeneratedClip,
                          .trackId = TrackId::create("title-1").value(),
                          .clipId = titleClip},
        true);

    EditorSessionWorker reopened;
    EditorSessionResultPtr reopenedResult;
    QObject::connect(
        &reopened, &EditorSessionWorker::opened, &reopened,
        [&](quint64, EditorSessionResultPtr value) {
            reopenedResult = std::move(value);
        },
        Qt::DirectConnection);
    reopened.openProject(21, package.path());
    ASSERT_NE(reopenedResult, nullptr);
    ASSERT_TRUE(reopenedResult->hasValue()) << reopenedResult->error().message();
    EXPECT_EQ(reopenedResult->value().state.snapshot.timeline,
              editedResult->value().state.snapshot.timeline);
    EXPECT_EQ(reopenedResult->value().state.snapshot.revision.value(),
              expectedRevision);
}

TEST(EditorSessionWorkerTest, PublishesCommittedEditWithDerivedWorkDiagnostic) {
    TempPackage package;
    package.seedOneClipTimeline();
    creator::app::GeneratedOverlayFactory derivedFailure =
        [](const creator::edit_engine::TimelineSnapshot&)
        -> creator::core::Result<std::vector<
            creator::edit_engine::GeneratedOverlayDescriptor>> {
        return creator::core::AppError{creator::core::ErrorCode::IoFailure,
                                       "injected derived work failure"};
    };
    EditorSessionWorker worker{
        std::make_unique<ProjectPackageStore>(),
        [](const fs::path& databasePath,
           const creator::domain::ProjectId& projectId)
        -> creator::core::Result<
            std::unique_ptr<creator::project_store::ITimelineStore>> {
            auto store = SqliteTimelineStore::open(databasePath, projectId);
            if (!store.hasValue()) return store.error();
            return std::unique_ptr<creator::project_store::ITimelineStore>{
                std::make_unique<SqliteTimelineStore>(std::move(store).value())};
        },
        std::move(derivedFailure)};
    EditorSessionResultPtr openedResult;
    EditorSessionResultPtr editedResult;
    QObject::connect(&worker, &EditorSessionWorker::opened, &worker,
                     [&](quint64, EditorSessionResultPtr value) {
                         openedResult = std::move(value);
                     });
    QObject::connect(&worker, &EditorSessionWorker::edited, &worker,
                     [&](quint64, quint64, EditorSessionResultPtr value) {
                         editedResult = std::move(value);
                     });
    worker.openProject(30, package.path());
    ASSERT_TRUE(openedResult->hasValue()) << openedResult->error().message();
    ASSERT_TRUE(openedResult->value().derivedWorkDiagnostic.has_value());

    worker.edit(30, 1,
                EditorEditRequest{
                    .kind = EditorEditKind::SetVisualTransform,
                    .trackId = TrackId::create("video-1").value(),
                    .clipId = ClipId::create("clip-1").value(),
                    .visualTransform = VisualTransform::create(
                        0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0,
                        0.0, 0.0, 1.0, 0)
                                           .value(),
                });
    ASSERT_NE(editedResult, nullptr);
    ASSERT_TRUE(editedResult->hasValue()) << editedResult->error().message();
    EXPECT_EQ(editedResult->value().state.snapshot.revision.value(), 1);
    ASSERT_TRUE(editedResult->value().derivedWorkDiagnostic.has_value());
    EXPECT_EQ(editedResult->value().derivedWorkDiagnostic->code(),
              creator::core::ErrorCode::IoFailure);
}

TEST(EditorSessionWorkerTest, PersistsSplitUndoAndReopensExactHistoryState) {
    TempPackage package;
    package.seedOneClipTimeline();
    QThread workerThread;
    auto* worker = new EditorSessionWorker;
    worker->moveToThread(&workerThread);
    QObject::connect(&workerThread, &QThread::finished, worker,
                     &QObject::deleteLater);

    std::mutex resultMutex;
    EditorSessionResultPtr openedResult;
    EditorSessionResultPtr editedResult;
    quint64 editedCommand = 0;
    QObject::connect(
        worker, &EditorSessionWorker::opened, worker,
        [&](quint64, EditorSessionResultPtr value) {
            std::lock_guard lock{resultMutex};
            openedResult = std::move(value);
        },
        Qt::DirectConnection);
    QObject::connect(
        worker, &EditorSessionWorker::edited, worker,
        [&](quint64, quint64 commandId, EditorSessionResultPtr value) {
            std::lock_guard lock{resultMutex};
            editedCommand = commandId;
            editedResult = std::move(value);
        },
        Qt::DirectConnection);

    workerThread.start();
    QMetaObject::invokeMethod(
        worker,
        [worker, path = package.path()] { worker->openProject(9, path); },
        Qt::QueuedConnection);
    ASSERT_TRUE(waitUntil([&] {
        std::lock_guard lock{resultMutex};
        return static_cast<bool>(openedResult);
    }));
    ASSERT_TRUE(openedResult->hasValue()) << openedResult->error().message();

    const EditorEditRequest split{
        .kind = EditorEditKind::Split,
        .trackId = TrackId::create("video-1").value(),
        .clipId = ClipId::create("clip-1").value(),
        .position = at(400),
    };
    QMetaObject::invokeMethod(
        worker, [worker, split] { worker->edit(9, 1, split); },
        Qt::QueuedConnection);
    ASSERT_TRUE(waitUntil([&] {
        std::lock_guard lock{resultMutex};
        return static_cast<bool>(editedResult);
    }));
    ASSERT_TRUE(editedResult->hasValue()) << editedResult->error().message();
    EXPECT_EQ(editedCommand, 1U);
    const auto& splitUpdate = editedResult->value();
    ASSERT_TRUE(splitUpdate.change.has_value());
    EXPECT_EQ(splitUpdate.change->baseRevision().value(), 0);
    EXPECT_EQ(splitUpdate.state.snapshot.revision.value(), 1);
    ASSERT_EQ(splitUpdate.state.snapshot.timeline.tracks()[0].clips().size(), 2U);
    EXPECT_TRUE(splitUpdate.state.canUndo);
    EXPECT_FALSE(splitUpdate.state.canRedo);
    EXPECT_FALSE(splitUpdate.state.clean);
    EXPECT_EQ(splitUpdate.state.historyCursor, 1U);

    {
        std::lock_guard lock{resultMutex};
        editedResult.reset();
    }
    const EditorEditRequest undo{.kind = EditorEditKind::Undo};
    QMetaObject::invokeMethod(
        worker, [worker, undo] { worker->edit(9, 2, undo); },
        Qt::QueuedConnection);
    ASSERT_TRUE(waitUntil([&] {
        std::lock_guard lock{resultMutex};
        return static_cast<bool>(editedResult);
    }));
    ASSERT_TRUE(editedResult->hasValue()) << editedResult->error().message();
    const auto finalState = editedResult->value().state;
    EXPECT_EQ(finalState.snapshot.revision.value(), 2);
    EXPECT_EQ(finalState.snapshot.timeline, oneClipTimeline());
    EXPECT_FALSE(finalState.canUndo);
    EXPECT_TRUE(finalState.canRedo);
    EXPECT_TRUE(finalState.clean);
    EXPECT_EQ(finalState.historyCursor, 0U);

    workerThread.quit();
    ASSERT_TRUE(workerThread.wait(3000));

    EditorSessionWorker reopenedWorker;
    EditorSessionResultPtr reopenedResult;
    QObject::connect(
        &reopenedWorker, &EditorSessionWorker::opened, &reopenedWorker,
        [&](quint64, EditorSessionResultPtr value) {
            reopenedResult = std::move(value);
        },
        Qt::DirectConnection);
    reopenedWorker.openProject(10, package.path());
    ASSERT_NE(reopenedResult, nullptr);
    ASSERT_TRUE(reopenedResult->hasValue()) << reopenedResult->error().message();
    const auto& reopenedState = reopenedResult->value().state;
    EXPECT_EQ(reopenedState.snapshot, finalState.snapshot);
    EXPECT_EQ(reopenedState.canUndo, finalState.canUndo);
    EXPECT_EQ(reopenedState.canRedo, finalState.canRedo);
    EXPECT_EQ(reopenedState.clean, finalState.clean);
    EXPECT_EQ(reopenedState.historyCursor, finalState.historyCursor);
}

TEST(EditorSessionWorkerTest, FailedCommitLeavesPublishedSessionExactlyUnchanged) {
    TempPackage package;
    FailingCommitStore* failingStore = nullptr;
    TimelineStoreFactory factory =
        [&](const fs::path&, const creator::domain::ProjectId&)
        -> creator::core::Result<
            std::unique_ptr<creator::project_store::ITimelineStore>> {
        auto store = std::make_unique<FailingCommitStore>(oneClipTimeline());
        failingStore = store.get();
        return std::unique_ptr<creator::project_store::ITimelineStore>{
            std::move(store)};
    };
    EditorSessionWorker worker{std::make_unique<ProjectPackageStore>(),
                               std::move(factory)};
    EditorSessionResultPtr openedResult;
    EditorSessionResultPtr editedResult;
    QObject::connect(
        &worker, &EditorSessionWorker::opened, &worker,
        [&](quint64, EditorSessionResultPtr value) {
            openedResult = std::move(value);
        },
        Qt::DirectConnection);
    QObject::connect(
        &worker, &EditorSessionWorker::edited, &worker,
        [&](quint64, quint64, EditorSessionResultPtr value) {
            editedResult = std::move(value);
        },
        Qt::DirectConnection);

    worker.openProject(3, package.path());
    ASSERT_NE(openedResult, nullptr);
    ASSERT_TRUE(openedResult->hasValue()) << openedResult->error().message();
    const auto initial = openedResult->value().state;
    ASSERT_NE(failingStore, nullptr);

    worker.edit(3, 1,
                EditorEditRequest{
                    .kind = EditorEditKind::Split,
                    .trackId = TrackId::create("video-1").value(),
                    .clipId = ClipId::create("clip-1").value(),
                    .position = at(400),
                });
    ASSERT_NE(editedResult, nullptr);
    ASSERT_FALSE(editedResult->hasValue());
    EXPECT_EQ(editedResult->error().code(),
              creator::core::ErrorCode::IoFailure);
    EXPECT_EQ(failingStore->commitCalls, 1U);

    editedResult.reset();
    worker.edit(3, 2, EditorEditRequest{.kind = EditorEditKind::Save});
    ASSERT_NE(editedResult, nullptr);
    ASSERT_TRUE(editedResult->hasValue()) << editedResult->error().message();
    const auto& afterFailure = editedResult->value().state;
    EXPECT_EQ(afterFailure.snapshot, initial.snapshot);
    EXPECT_EQ(afterFailure.canUndo, initial.canUndo);
    EXPECT_EQ(afterFailure.canRedo, initial.canRedo);
    EXPECT_EQ(afterFailure.clean, initial.clean);
}

TEST(EditorSessionWorkerTest, StaleSelectionAndLockedTrackNeverReachCommit) {
    TempPackage package;
    auto lockedTimeline = oneClipTimeline();
    ASSERT_TRUE(lockedTimeline
                    .setTrackLocked(TrackId::create("video-1").value(), true)
                    .hasValue());
    FailingCommitStore* storeView = nullptr;
    TimelineStoreFactory factory =
        [&](const fs::path&, const creator::domain::ProjectId&)
        -> creator::core::Result<
            std::unique_ptr<creator::project_store::ITimelineStore>> {
        auto store =
            std::make_unique<FailingCommitStore>(std::move(lockedTimeline));
        storeView = store.get();
        return std::unique_ptr<creator::project_store::ITimelineStore>{
            std::move(store)};
    };
    EditorSessionWorker worker{std::make_unique<ProjectPackageStore>(),
                               std::move(factory)};
    EditorSessionResultPtr openedResult;
    EditorSessionResultPtr editedResult;
    QObject::connect(&worker, &EditorSessionWorker::opened, &worker,
                     [&](quint64, EditorSessionResultPtr value) {
                         openedResult = std::move(value);
                     });
    QObject::connect(&worker, &EditorSessionWorker::edited, &worker,
                     [&](quint64, quint64, EditorSessionResultPtr value) {
                         editedResult = std::move(value);
                     });
    worker.openProject(31, package.path());
    ASSERT_TRUE(openedResult->hasValue()) << openedResult->error().message();
    ASSERT_NE(storeView, nullptr);

    worker.edit(31, 1,
                EditorEditRequest{
                    .kind = EditorEditKind::SetVisualTransform,
                    .trackId = TrackId::create("video-1").value(),
                    .clipId = ClipId::create("missing-clip").value(),
                });
    ASSERT_NE(editedResult, nullptr);
    EXPECT_FALSE(editedResult->hasValue());
    EXPECT_EQ(storeView->commitCalls, 0U);

    editedResult.reset();
    worker.edit(31, 2,
                EditorEditRequest{
                    .kind = EditorEditKind::SetVisualTransform,
                    .trackId = TrackId::create("video-1").value(),
                    .clipId = ClipId::create("clip-1").value(),
                });
    ASSERT_NE(editedResult, nullptr);
    EXPECT_FALSE(editedResult->hasValue());
    EXPECT_EQ(storeView->commitCalls, 0U);
}

TEST(EditorSessionWorkerTest, ExecutesTrimDeleteRedoAndExplicitSaveDurably) {
    TempPackage package;
    package.seedOneClipTimeline();
    EditorSessionWorker worker;
    EditorSessionResultPtr openedResult;
    EditorSessionResultPtr editedResult;
    QObject::connect(
        &worker, &EditorSessionWorker::opened, &worker,
        [&](quint64, EditorSessionResultPtr value) {
            openedResult = std::move(value);
        },
        Qt::DirectConnection);
    QObject::connect(
        &worker, &EditorSessionWorker::edited, &worker,
        [&](quint64, quint64, EditorSessionResultPtr value) {
            editedResult = std::move(value);
        },
        Qt::DirectConnection);
    worker.openProject(4, package.path());
    ASSERT_TRUE(openedResult->hasValue()) << openedResult->error().message();

    const auto run = [&](quint64 commandId, EditorEditRequest request) {
        editedResult.reset();
        worker.edit(4, commandId, std::move(request));
        EXPECT_NE(editedResult, nullptr);
        EXPECT_TRUE(editedResult->hasValue())
            << (editedResult->hasValue() ? "" : editedResult->error().message());
    };
    const auto trackId = TrackId::create("video-1").value();
    const auto clipId = ClipId::create("clip-1").value();

    run(1, EditorEditRequest{.kind = EditorEditKind::TrimLeading,
                             .trackId = trackId,
                             .clipId = clipId,
                             .position = at(100)});
    ASSERT_TRUE(editedResult->hasValue());
    auto state = editedResult->value().state;
    ASSERT_EQ(state.snapshot.timeline.tracks()[0].clips().size(), 1U);
    EXPECT_EQ(state.snapshot.timeline.tracks()[0].clips()[0]
                  .timelineRange()
                  .start(),
              at(100));
    EXPECT_EQ(state.snapshot.revision.value(), 1);

    run(2, EditorEditRequest{.kind = EditorEditKind::TrimTrailing,
                             .trackId = trackId,
                             .clipId = clipId,
                             .position = at(900)});
    ASSERT_TRUE(editedResult->hasValue());
    state = editedResult->value().state;
    EXPECT_EQ(state.snapshot.timeline.tracks()[0].clips()[0]
                  .timelineRange()
                  .duration(),
              DurationNs{800});
    EXPECT_EQ(state.snapshot.revision.value(), 2);

    run(3, EditorEditRequest{
               .kind = EditorEditKind::DeleteRange,
               .range = TimeRange::create(at(300), DurationNs{200}).value(),
               .ripple = false,
           });
    ASSERT_TRUE(editedResult->hasValue());
    state = editedResult->value().state;
    ASSERT_EQ(state.snapshot.timeline.tracks()[0].clips().size(), 2U);
    EXPECT_EQ(state.snapshot.revision.value(), 3);
    ASSERT_TRUE(editedResult->value().change.has_value());
    EXPECT_TRUE(editedResult->value().change->requiresFullRebuild());
    EXPECT_TRUE(editedResult->value().change->affectedTracks().empty());

    run(4, EditorEditRequest{.kind = EditorEditKind::Undo});
    ASSERT_TRUE(editedResult->hasValue());
    EXPECT_EQ(editedResult->value().state.snapshot.revision.value(), 4);
    EXPECT_EQ(editedResult->value().state.snapshot.timeline.tracks()[0]
                  .clips()
                  .size(),
              1U);

    run(5, EditorEditRequest{.kind = EditorEditKind::Redo});
    ASSERT_TRUE(editedResult->hasValue());
    EXPECT_EQ(editedResult->value().state.snapshot.revision.value(), 5);
    EXPECT_EQ(editedResult->value().state.snapshot.timeline.tracks()[0]
                  .clips()
                  .size(),
              2U);
    ASSERT_TRUE(editedResult->value().change.has_value());
    EXPECT_TRUE(editedResult->value().change->requiresFullRebuild());

    run(6, EditorEditRequest{.kind = EditorEditKind::Save});
    ASSERT_TRUE(editedResult->hasValue());
    const auto saved = editedResult->value().state;
    EXPECT_EQ(saved.snapshot.revision.value(), 5);
    EXPECT_TRUE(saved.clean);
    EXPECT_TRUE(saved.canUndo);
    EXPECT_FALSE(saved.canRedo);
    EXPECT_FALSE(editedResult->value().change.has_value());

    EditorSessionWorker reopenedWorker;
    EditorSessionResultPtr reopenedResult;
    QObject::connect(
        &reopenedWorker, &EditorSessionWorker::opened, &reopenedWorker,
        [&](quint64, EditorSessionResultPtr value) {
            reopenedResult = std::move(value);
        },
        Qt::DirectConnection);
    reopenedWorker.openProject(5, package.path());
    ASSERT_TRUE(reopenedResult->hasValue()) << reopenedResult->error().message();
    EXPECT_EQ(reopenedResult->value().state.snapshot, saved.snapshot);
    EXPECT_EQ(reopenedResult->value().state.clean, saved.clean);
    EXPECT_EQ(reopenedResult->value().state.canUndo, saved.canUndo);
    EXPECT_EQ(reopenedResult->value().state.canRedo, saved.canRedo);
}

TEST(EditorSessionWorkerTest, RejectsDatabaseThatWasReplacedByDirectory) {
    TempPackage package;
    ProjectPackageStore packageStore;
    auto opened = packageStore.open(package.path());
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    const auto database =
        package.path() / opened.value().package.manifest.database;
    ASSERT_TRUE(fs::remove(database));
    ASSERT_TRUE(fs::create_directory(database));

    EditorSessionWorker worker;
    EditorSessionResultPtr result;
    QObject::connect(
        &worker, &EditorSessionWorker::opened, &worker,
        [&](quint64, EditorSessionResultPtr value) { result = std::move(value); },
        Qt::DirectConnection);
    worker.openProject(6, package.path());

    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(result->hasValue());
    EXPECT_EQ(result->error().code(),
              creator::core::ErrorCode::InvalidArgument);
}

TEST(EditorSessionWorkerTest, LargeDeletePublishesFullRebuildAfterDurableCommit) {
    TempPackage package;
    ProjectPackageStore packageStore;
    auto opened = packageStore.open(package.path());
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto storeResult = SqliteTimelineStore::open(
        package.path() / opened.value().package.manifest.database,
        opened.value().package.manifest.projectId);
    ASSERT_TRUE(storeResult.hasValue()) << storeResult.error().message();
    auto store = std::move(storeResult).value();
    auto timeline = Timeline::create(TimelineId::create("large").value(), "Large",
                                     FrameRate::create(60, 1).value())
                        .value();
    for (std::size_t index = 0; index < 257; ++index) {
        ASSERT_TRUE(timeline.addTrack(
                                Track::create(
                                    TrackId::create("track-" +
                                                    std::to_string(index))
                                        .value(),
                                    TrackKind::Video, "Track", true, false)
                                    .value())
                        .hasValue());
    }
    ASSERT_TRUE(store.createTimeline(timeline).hasValue());

    EditorSessionWorker worker;
    EditorSessionResultPtr openedResult;
    EditorSessionResultPtr editedResult;
    QObject::connect(
        &worker, &EditorSessionWorker::opened, &worker,
        [&](quint64, EditorSessionResultPtr value) {
            openedResult = std::move(value);
        },
        Qt::DirectConnection);
    QObject::connect(
        &worker, &EditorSessionWorker::edited, &worker,
        [&](quint64, quint64, EditorSessionResultPtr value) {
            editedResult = std::move(value);
        },
        Qt::DirectConnection);
    worker.openProject(8, package.path());
    ASSERT_TRUE(openedResult->hasValue()) << openedResult->error().message();
    worker.edit(8, 1,
                EditorEditRequest{
                    .kind = EditorEditKind::DeleteRange,
                    .range = TimeRange::create(at(10), DurationNs{10}).value(),
                    .ripple = true,
                });

    ASSERT_NE(editedResult, nullptr);
    ASSERT_TRUE(editedResult->hasValue()) << editedResult->error().message();
    ASSERT_TRUE(editedResult->value().change.has_value());
    EXPECT_TRUE(editedResult->value().change->requiresFullRebuild());
    EXPECT_TRUE(editedResult->value().change->affectedTracks().empty());
    EXPECT_EQ(editedResult->value().state.snapshot.revision.value(), 1);
}

}  // namespace
