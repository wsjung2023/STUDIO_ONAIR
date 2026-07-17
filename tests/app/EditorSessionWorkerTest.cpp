#include "app/EditorSessionWorker.h"

#include "domain/MediaAsset.h"
#include "domain/Timeline.h"
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
using creator::domain::VideoAssetMetadata;
using creator::project_store::ProjectPackageStore;
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
               std::nullopt, 100, "fingerprint", AssetAvailability::Available)
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
