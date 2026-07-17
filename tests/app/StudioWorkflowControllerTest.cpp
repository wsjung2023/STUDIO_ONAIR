#include "app/StudioWorkflowController.h"

#include "core/AppError.h"
#include "domain/StudioScene.h"
#include "project_store/IStudioStore.h"
#include "project_store/ProjectPackageStore.h"
#include "project_store/SqliteStudioStore.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QSignalSpy>
#include <QUrl>

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using creator::app::IRecordingTimelineReconciler;
using creator::app::RecordingReconcileResult;
using creator::app::StudioWorkflowController;
using creator::core::AppError;
using creator::core::ErrorCode;
using creator::core::Result;
using creator::domain::SessionId;
using creator::project_store::IStudioStore;
using creator::project_store::RecordingImportRecord;
using creator::project_store::RecordingMarker;
using creator::project_store::RecordingSceneEvent;
using creator::project_store::RecordingSourceRole;
using creator::project_store::StudioSnapshot;
using creator::project_store::UnimportedRecording;

bool waitUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout = std::chrono::seconds{3}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

StudioSnapshot defaultSnapshot() {
    auto defaults = creator::domain::defaultStudioScenes();
    return StudioSnapshot{.scenes = std::move(defaults).value(),
                          .activeSceneId =
                              creator::domain::SceneId::create("presentation")
                                  .value()};
}

struct SharedStoreState final {
    SharedStoreState() : snapshot(defaultSnapshot()) {}

    void deferNextMutation() {
        std::scoped_lock lock{mutex};
        defer = true;
        released = false;
        entered = false;
        fail = false;
    }

    void failNextMutation() {
        std::scoped_lock lock{mutex};
        defer = false;
        fail = true;
    }

    void deferNextLoad() {
        std::scoped_lock lock{mutex};
        deferLoad = true;
        loadReleased = false;
        loadEntered = false;
    }

    bool waitUntilLoadEntered() {
        std::unique_lock lock{mutex};
        return condition.wait_for(lock, std::chrono::seconds{3},
                                  [&] { return loadEntered; });
    }

    void finishLoad() {
        {
            std::scoped_lock lock{mutex};
            loadReleased = true;
        }
        condition.notify_all();
    }

    bool waitUntilEntered() {
        std::unique_lock lock{mutex};
        return condition.wait_for(lock, std::chrono::seconds{3},
                                  [&] { return entered; });
    }

    void finishMutation() {
        {
            std::scoped_lock lock{mutex};
            released = true;
        }
        condition.notify_all();
    }

    std::mutex mutex;
    std::condition_variable condition;
    StudioSnapshot snapshot;
    bool defer{false};
    bool released{false};
    bool entered{false};
    bool fail{false};
    bool seedFailure{false};
    bool markerFailure{false};
    bool deferLoad{false};
    bool loadReleased{false};
    bool loadEntered{false};
    std::size_t mutationCount{0};
    std::size_t storeDestructionCount{0};
    std::vector<RecordingSceneEvent> sceneEvents;
    std::vector<RecordingMarker> markers;
    std::vector<UnimportedRecording> unimported;
};

class SharedStudioStore final : public IStudioStore {
public:
    explicit SharedStudioStore(std::shared_ptr<SharedStoreState> state)
        : state_(std::move(state)) {}

    ~SharedStudioStore() override {
        std::scoped_lock lock{state_->mutex};
        ++state_->storeDestructionCount;
    }

    Result<void> seedDefaultsIfEmpty(
        const std::vector<creator::domain::StudioScene>&) override {
        std::scoped_lock lock{state_->mutex};
        if (state_->seedFailure) {
            return AppError{ErrorCode::IoFailure, "injected seed failure"};
        }
        return creator::core::ok();
    }

    Result<StudioSnapshot> load() override {
        std::unique_lock lock{state_->mutex};
        state_->loadEntered = true;
        state_->condition.notify_all();
        if (state_->deferLoad) {
            state_->condition.wait(lock, [&] { return state_->loadReleased; });
            state_->deferLoad = false;
        }
        return state_->snapshot;
    }

    Result<void> commitSceneMutation(const StudioSnapshot& snapshot) override {
        std::unique_lock lock{state_->mutex};
        state_->entered = true;
        state_->condition.notify_all();
        if (state_->defer) {
            state_->condition.wait(lock, [&] { return state_->released; });
            state_->defer = false;
        }
        if (std::exchange(state_->fail, false)) {
            return AppError{ErrorCode::IoFailure, "injected scene failure"};
        }
        state_->snapshot = snapshot;
        ++state_->mutationCount;
        return creator::core::ok();
    }

    Result<void> prepareRecording(
        const SessionId&, const std::vector<RecordingSourceRole>&,
        const creator::domain::SceneId&) override {
        return creator::core::ok();
    }
    Result<void> discardRecordingPreparation(const SessionId&) override {
        return creator::core::ok();
    }
    Result<void> recordSceneSwitch(
        const SessionId& sessionId, const creator::domain::SceneId& sceneId,
        std::uint64_t sequence, creator::core::TimestampNs position) override {
        std::scoped_lock lock{state_->mutex};
        state_->sceneEvents.push_back({.sessionId = sessionId,
                                       .sequence = sequence,
                                       .sceneId = sceneId,
                                       .position = position});
        return creator::core::ok();
    }
    Result<void> recordMarker(const RecordingMarker& marker) override {
        std::scoped_lock lock{state_->mutex};
        if (std::exchange(state_->markerFailure, false)) {
            return AppError{ErrorCode::IoFailure, "injected marker failure"};
        }
        state_->markers.push_back(marker);
        return creator::core::ok();
    }
    Result<std::vector<RecordingSourceRole>> loadRecordingSources(
        const SessionId&) override {
        return std::vector<RecordingSourceRole>{};
    }
    Result<std::vector<RecordingSceneEvent>> loadRecordingSceneEvents(
        const SessionId&) override {
        return std::vector<RecordingSceneEvent>{};
    }
    Result<std::vector<RecordingMarker>> loadRecordingMarkers(
        const SessionId&) override {
        return std::vector<RecordingMarker>{};
    }
    Result<std::vector<UnimportedRecording>> completedUnimportedRecordings()
        override {
        std::scoped_lock lock{state_->mutex};
        return state_->unimported;
    }
    Result<void> putRecordingImport(const RecordingImportRecord&) override {
        return creator::core::ok();
    }
    Result<std::optional<RecordingImportRecord>> recordingImport(
        const SessionId&) override {
        return std::optional<RecordingImportRecord>{};
    }

private:
    std::shared_ptr<SharedStoreState> state_;
};

class NoopReconciler final : public IRecordingTimelineReconciler {
public:
    Result<RecordingReconcileResult> reconcile(
        const std::filesystem::path&, const SessionId& sessionId) override {
        return RecordingReconcileResult{.sessionId = sessionId,
                                        .imported = true,
                                        .revision = 1,
                                        .assetCount = 0,
                                        .trackCount = 0,
                                        .markerCount = 0};
    }
};

struct SharedReconcilerState final {
    void deferNext() {
        std::scoped_lock lock{mutex};
        defer = true;
        entered = false;
        released = false;
    }

    bool waitUntilEntered() {
        std::unique_lock lock{mutex};
        return condition.wait_for(lock, std::chrono::seconds{3},
                                  [&] { return entered; });
    }

    void finish() {
        {
            std::scoped_lock lock{mutex};
            released = true;
        }
        condition.notify_all();
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::size_t calls{0};
    bool fail{false};
    bool defer{false};
    bool entered{false};
    bool released{false};
};

class SharedReconciler final : public IRecordingTimelineReconciler {
public:
    explicit SharedReconciler(std::shared_ptr<SharedReconcilerState> state)
        : state_(std::move(state)) {}

    Result<RecordingReconcileResult> reconcile(
        const std::filesystem::path&, const SessionId& sessionId) override {
        std::unique_lock lock{state_->mutex};
        ++state_->calls;
        state_->entered = true;
        state_->condition.notify_all();
        if (state_->defer) {
            state_->condition.wait(lock, [&] { return state_->released; });
            state_->defer = false;
        }
        if (state_->fail) {
            return AppError{ErrorCode::IoFailure,
                            "injected reconciliation failure"};
        }
        return RecordingReconcileResult{.sessionId = sessionId,
                                        .imported = true,
                                        .revision = 1,
                                        .assetCount = 0,
                                        .trackCount = 0,
                                        .markerCount = 0};
    }

private:
    std::shared_ptr<SharedReconcilerState> state_;
};

StudioWorkflowController makeController(
    const std::shared_ptr<SharedStoreState>& state,
    std::shared_ptr<SharedReconcilerState> reconcilerState = {}) {
    std::unique_ptr<IRecordingTimelineReconciler> reconciler =
        reconcilerState
            ? std::unique_ptr<IRecordingTimelineReconciler>{
                  new SharedReconciler{std::move(reconcilerState)}}
            : std::unique_ptr<IRecordingTimelineReconciler>{
                  new NoopReconciler{}};
    return StudioWorkflowController{
        [state](const std::filesystem::path&)
            -> Result<std::unique_ptr<IStudioStore>> {
            return std::unique_ptr<IStudioStore>{new SharedStudioStore{state}};
        },
        std::move(reconciler),
        [next = std::uint64_t{0}]() mutable {
            return "studio-id-" + std::to_string(++next);
        }};
}

void openReady(StudioWorkflowController& controller) {
    controller.openProject(QUrl::fromLocalFile(QStringLiteral("C:/project")));
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    ASSERT_EQ(controller.sceneModel()->rowCount(), 3);
}

TEST(StudioWorkflowControllerTest,
     SceneSwitchPublishesOnlyAfterDurableMutationCompletes) {
    auto state = std::make_shared<SharedStoreState>();
    auto controller = makeController(state);
    openReady(controller);
    state->deferNextMutation();

    controller.switchScene(QStringLiteral("screen"));

    ASSERT_TRUE(state->waitUntilEntered());
    EXPECT_TRUE(controller.busy());
    EXPECT_EQ(controller.activeSceneId(), QStringLiteral("presentation"));
    controller.selectScene(QStringLiteral("camera"));
    EXPECT_EQ(controller.selectedSceneId(), QStringLiteral("camera"));
    state->finishMutation();
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    EXPECT_EQ(controller.activeSceneId(), QStringLiteral("screen"));
    EXPECT_EQ(controller.selectedSceneId(), QStringLiteral("camera"));
}

TEST(StudioWorkflowControllerTest,
     FailedSceneSwitchLeavesPublishedSnapshotExactlyUnchanged) {
    auto state = std::make_shared<SharedStoreState>();
    auto controller = makeController(state);
    openReady(controller);
    state->failNextMutation();

    controller.switchScene(QStringLiteral("camera"));

    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    EXPECT_EQ(controller.activeSceneId(), QStringLiteral("presentation"));
    EXPECT_TRUE(controller.statusMessage().contains(
        QStringLiteral("injected scene failure")));
    EXPECT_EQ(controller.sceneModel()->rowCount(), 3);
}

TEST(StudioWorkflowControllerTest,
     RecordingEventsKeepSequenceAndMarkerFailureKeepsPublishedCount) {
    auto state = std::make_shared<SharedStoreState>();
    auto reconciler = std::make_shared<SharedReconcilerState>();
    auto controller = makeController(state, reconciler);
    openReady(controller);
    const QVariantList sources{QVariantMap{
        {QStringLiteral("sourceId"), QStringLiteral("screen")},
        {QStringLiteral("role"), QStringLiteral("screen")}}};

    controller.prepareRecording(QStringLiteral("session-1"), sources, 0);
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    ASSERT_TRUE(controller.recording());
    controller.switchScene(QStringLiteral("screen"), 100);
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    controller.switchScene(QStringLiteral("camera"), 250);
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    controller.switchScene(QStringLiteral("screen"), 200);
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    EXPECT_EQ(controller.activeSceneId(), QStringLiteral("camera"));
    EXPECT_TRUE(controller.statusMessage().contains(
        QStringLiteral("position must be monotonic")));
    {
        std::scoped_lock lock{state->mutex};
        ASSERT_EQ(state->sceneEvents.size(), 2U);
        EXPECT_EQ(state->sceneEvents[0].sequence, 1U);
        EXPECT_EQ(state->sceneEvents[0].position.time_since_epoch().count(), 100);
        EXPECT_EQ(state->sceneEvents[1].sequence, 2U);
        EXPECT_EQ(state->sceneEvents[1].position.time_since_epoch().count(), 250);
        state->markerFailure = true;
    }
    QSignalSpy sceneResetSpy(controller.sceneModel(),
                             &QAbstractItemModel::modelReset);
    QSignalSpy sourceResetSpy(controller.sourceModel(),
                              &QAbstractItemModel::modelReset);

    controller.addMarker(QStringLiteral("Important"), 300);
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    EXPECT_EQ(controller.markerCount(), 0);
    EXPECT_TRUE(controller.statusMessage().contains(
        QStringLiteral("injected marker failure")));
    controller.addMarker(QString{}, 350);
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    EXPECT_EQ(controller.markerCount(), 1);
    EXPECT_EQ(controller.recordingPositionNs(), 350);
    EXPECT_EQ(sceneResetSpy.count(), 0);
    EXPECT_EQ(sourceResetSpy.count(), 0);
    {
        std::scoped_lock lock{state->mutex};
        ASSERT_EQ(state->markers.size(), 1U);
        EXPECT_TRUE(state->markers.front().label.empty());
    }

    {
        std::scoped_lock lock{reconciler->mutex};
        reconciler->fail = true;
    }
    controller.completeRecording();
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    EXPECT_FALSE(controller.recording());
    EXPECT_TRUE(controller.statusMessage().contains(
        QStringLiteral("timeline reconciliation failed")));
    {
        std::scoped_lock lock{reconciler->mutex};
        EXPECT_EQ(reconciler->calls, 1U);
        reconciler->fail = false;
    }
    controller.retryReconciliation();
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    EXPECT_TRUE(controller.statusMessage().isEmpty());
    {
        std::scoped_lock lock{reconciler->mutex};
        EXPECT_EQ(reconciler->calls, 2U);
    }
}

TEST(StudioWorkflowControllerTest,
     OpenFailureForPendingImportKeepsStudioUsableAndRetryable) {
    auto state = std::make_shared<SharedStoreState>();
    auto reconciler = std::make_shared<SharedReconcilerState>();
    const auto session = SessionId::create("pending-session").value();
    state->unimported.push_back(
        {.sessionId = session,
         .startedAt = creator::core::TimestampNs{},
         .stoppedAt = creator::core::TimestampNs{
             creator::core::DurationNs{1'000}}});
    state->unimported.push_back(
        {.sessionId = SessionId::create("pending-session-2").value(),
         .startedAt = creator::core::TimestampNs{
             creator::core::DurationNs{2'000}},
         .stoppedAt = creator::core::TimestampNs{
             creator::core::DurationNs{3'000}}});
    reconciler->fail = true;
    auto controller = makeController(state, reconciler);

    openReady(controller);

    EXPECT_TRUE(controller.statusMessage().contains(
        QStringLiteral("injected reconciliation failure")));
    EXPECT_EQ(controller.sceneModel()->rowCount(), 3);
    {
        std::scoped_lock lock{reconciler->mutex};
        EXPECT_EQ(reconciler->calls, 1U);
        reconciler->fail = false;
    }
    controller.retryReconciliation();
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    EXPECT_TRUE(controller.statusMessage().isEmpty());
    {
        std::scoped_lock lock{reconciler->mutex};
        EXPECT_EQ(reconciler->calls, 3U);
    }
}

TEST(StudioWorkflowControllerTest,
     AutomaticOpenReconciliationPublishesProgressWithoutBlockingUi) {
    auto state = std::make_shared<SharedStoreState>();
    auto reconciler = std::make_shared<SharedReconcilerState>();
    state->unimported.push_back(
        {.sessionId = SessionId::create("delayed-session").value(),
         .startedAt = creator::core::TimestampNs{},
         .stoppedAt = creator::core::TimestampNs{
             creator::core::DurationNs{1'000}}});
    reconciler->deferNext();
    auto controller = makeController(state, reconciler);

    controller.openProject(QUrl::fromLocalFile(QStringLiteral("C:/project")));

    ASSERT_TRUE(reconciler->waitUntilEntered());
    ASSERT_TRUE(waitUntil([&] { return controller.reconciling(); }));
    EXPECT_TRUE(controller.busy());
    EXPECT_EQ(controller.sceneModel()->rowCount(), 0);
    reconciler->finish();
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    EXPECT_FALSE(controller.reconciling());
    EXPECT_EQ(controller.sceneModel()->rowCount(), 3);
}

TEST(StudioWorkflowControllerTest, RecordingGuardsEveryLayoutMutation) {
    auto state = std::make_shared<SharedStoreState>();
    auto controller = makeController(state);
    openReady(controller);
    const QVariantList sources{QVariantMap{
        {QStringLiteral("sourceId"), QStringLiteral("screen")},
        {QStringLiteral("role"), QStringLiteral("screen")}}};
    controller.prepareRecording(QStringLiteral("session-guard"), sources, 0);
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));

    const auto rejected = [&](const std::function<void()>& action) {
        action();
        ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
        EXPECT_TRUE(controller.statusMessage().contains(
            QStringLiteral("read-only while recording")));
    };
    rejected([&] { controller.addScene(QStringLiteral("Blocked")); });
    rejected([&] { controller.duplicateSelectedScene(); });
    rejected([&] {
        controller.renameScene(QStringLiteral("presentation"),
                               QStringLiteral("Blocked"));
    });
    rejected([&] { controller.removeScene(QStringLiteral("screen")); });
    rejected([&] { controller.moveScene(QStringLiteral("screen"), 1); });
    rejected([&] { controller.toggleSource(QStringLiteral("screen")); });
    rejected([&] { controller.moveSource(QStringLiteral("camera"), 1); });
    controller.selectSource(QStringLiteral("camera"));
    rejected([&] {
        controller.setSelectedTransform(0.1, 0.1, 0.5, 0.5, 1.0, 1.0, 0.0,
                                        0.0, 0.0, 0.0, 0.0, 1.0, 5);
    });
    rejected([&] { controller.resetSelectedTransform(); });
    rejected([&] {
        controller.applySelectedPipPreset(QStringLiteral("bottom-right"));
    });
    {
        std::scoped_lock lock{state->mutex};
        EXPECT_EQ(state->mutationCount, 0U);
    }
    EXPECT_EQ(controller.sceneModel()->rowCount(), 3);
    EXPECT_EQ(controller.activeSceneId(), QStringLiteral("presentation"));
}

TEST(StudioWorkflowControllerTest,
     RecordingRejectsProjectOpenWithoutForgettingActiveSession) {
    auto state = std::make_shared<SharedStoreState>();
    auto controller = makeController(state);
    openReady(controller);
    const QVariantList sources{QVariantMap{
        {QStringLiteral("sourceId"), QStringLiteral("screen")},
        {QStringLiteral("role"), QStringLiteral("screen")}}};
    controller.prepareRecording(QStringLiteral("session-open-guard"), sources,
                                0);
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));

    controller.openProject(QUrl::fromLocalFile(QStringLiteral("C:/other")));

    EXPECT_TRUE(controller.recording());
    EXPECT_EQ(controller.activeSessionId(),
              QStringLiteral("session-open-guard"));
    EXPECT_EQ(controller.sceneModel()->rowCount(), 3);
    EXPECT_TRUE(controller.statusMessage().contains(
        QStringLiteral("Stop the active Studio operation")));
    controller.reopenProject();
    EXPECT_TRUE(controller.recording());
    EXPECT_EQ(controller.activeSessionId(),
              QStringLiteral("session-open-guard"));
}

TEST(StudioWorkflowControllerTest, SeedFailurePublishesNoPartialModels) {
    auto state = std::make_shared<SharedStoreState>();
    state->seedFailure = true;
    auto controller = makeController(state);

    controller.openProject(QUrl::fromLocalFile(QStringLiteral("C:/project")));

    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    EXPECT_EQ(controller.sceneModel()->rowCount(), 0);
    EXPECT_EQ(controller.sourceModel()->rowCount(), 0);
    EXPECT_TRUE(controller.statusMessage().contains(
        QStringLiteral("injected seed failure")));
}

TEST(StudioWorkflowControllerTest, StaleOpenCompletionCannotReplaceNewProject) {
    auto first = std::make_shared<SharedStoreState>();
    auto second = std::make_shared<SharedStoreState>();
    second->snapshot.activeSceneId =
        creator::domain::SceneId::create("camera").value();
    first->deferNextLoad();
    auto openCount = std::make_shared<std::size_t>(0);
    StudioWorkflowController controller{
        [first, second, openCount](const std::filesystem::path&)
            -> Result<std::unique_ptr<IStudioStore>> {
            const auto current = (*openCount)++;
            return std::unique_ptr<IStudioStore>{new SharedStudioStore{
                current == 0 ? first : second}};
        },
        std::make_unique<NoopReconciler>(),
        [] { return std::string{"stale-id"}; }};

    controller.openProject(QUrl::fromLocalFile(QStringLiteral("C:/first")));
    ASSERT_TRUE(first->waitUntilLoadEntered());
    controller.openProject(QUrl::fromLocalFile(QStringLiteral("C:/second")));
    first->finishLoad();

    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    EXPECT_EQ(controller.activeSceneId(), QStringLiteral("camera"));
    EXPECT_EQ(controller.sceneModel()->rowCount(), 3);
}

TEST(StudioWorkflowControllerTest, EverySceneAndSourceEditPublishesDurableState) {
    auto state = std::make_shared<SharedStoreState>();
    auto controller = makeController(state);
    openReady(controller);
    const auto run = [&](const std::function<void()>& action) {
        action();
        ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
        ASSERT_TRUE(controller.statusMessage().isEmpty())
            << controller.statusMessage().toStdString();
    };

    run([&] { controller.addScene(QString::fromUtf8("강의 장면")); });
    EXPECT_EQ(controller.selectedSceneId(), QStringLiteral("studio-id-1"));
    EXPECT_EQ(controller.sceneModel()->rowCount(), 4);
    run([&] {
        controller.renameScene(QStringLiteral("studio-id-1"),
                               QString::fromUtf8("발표 장면"));
    });
    run([&] { controller.duplicateSelectedScene(); });
    EXPECT_EQ(controller.selectedSceneId(), QStringLiteral("studio-id-2"));
    EXPECT_EQ(controller.sceneModel()->rowCount(), 5);
    run([&] { controller.moveScene(QStringLiteral("studio-id-2"), -1); });
    run([&] { controller.removeScene(QStringLiteral("studio-id-1")); });
    EXPECT_EQ(controller.sceneModel()->rowCount(), 4);

    controller.selectScene(QStringLiteral("presentation"));
    controller.selectSource(QStringLiteral("camera"));
    run([&] { controller.toggleSource(QStringLiteral("camera")); });
    run([&] { controller.moveSource(QStringLiteral("camera"), -1); });
    QSignalSpy transformSpy(&controller,
                            &StudioWorkflowController::selectionChanged);
    run([&] {
        controller.setSelectedTransform(0.2, 0.3, 0.4, 0.5, 1.0, 1.0, 5.0,
                                        0.0, 0.0, 0.0, 0.0, 0.8, 7);
    });
    EXPECT_EQ(transformSpy.count(), 1);
    EXPECT_DOUBLE_EQ(controller.selectedTransform()
                         .value(QStringLiteral("opacity"))
                         .toDouble(),
                     0.8);
    run([&] {
        controller.applySelectedPipPreset(QStringLiteral("bottom-left"));
    });
    EXPECT_DOUBLE_EQ(controller.selectedTransform()
                         .value(QStringLiteral("y"))
                         .toDouble(),
                     0.70);
    run([&] { controller.resetSelectedTransform(); });
    EXPECT_DOUBLE_EQ(controller.selectedTransform()
                         .value(QStringLiteral("width"))
                         .toDouble(),
                     1.0);
    {
        std::scoped_lock lock{state->mutex};
        EXPECT_EQ(state->mutationCount, 10U);
        ASSERT_EQ(state->snapshot.scenes.size(), 4U);
        for (std::size_t index = 0; index < state->snapshot.scenes.size();
             ++index) {
            EXPECT_EQ(state->snapshot.scenes[index].position(),
                      static_cast<std::int32_t>(index));
        }
        const auto presentation = std::ranges::find(
            state->snapshot.scenes,
            creator::domain::SceneId::create("presentation").value(),
            &creator::domain::StudioScene::id);
        ASSERT_NE(presentation, state->snapshot.scenes.end());
        ASSERT_FALSE(presentation->sources().empty());
        EXPECT_EQ(presentation->sources().front().id().value(), "camera");
        EXPECT_FALSE(presentation->sources().front().enabled());
        ASSERT_TRUE(presentation->sources().front().transform().has_value());
        EXPECT_DOUBLE_EQ(
            presentation->sources().front().transform()->width(), 1.0);
    }
}

TEST(StudioWorkflowControllerTest, RepeatedOpenAndDestructionReleaseEveryStore) {
    auto state = std::make_shared<SharedStoreState>();
    {
        auto controller = makeController(state);
        openReady(controller);
        controller.reopenProject();
        ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
        EXPECT_EQ(controller.sceneModel()->rowCount(), 3);
    }
    std::scoped_lock lock{state->mutex};
    EXPECT_EQ(state->storeDestructionCount, 2U);
}

TEST(StudioWorkflowControllerTest, FailedReopenReleasesPreviousProjectStore) {
    auto state = std::make_shared<SharedStoreState>();
    auto controller = makeController(state);
    openReady(controller);
    {
        std::scoped_lock lock{state->mutex};
        state->seedFailure = true;
    }

    controller.openProject(QUrl::fromLocalFile(QStringLiteral("C:/failed")));

    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    EXPECT_EQ(controller.sceneModel()->rowCount(), 0);
    std::scoped_lock lock{state->mutex};
    EXPECT_EQ(state->storeDestructionCount, 2U);
}

TEST(StudioWorkflowControllerTest,
     RealPackageReopenRestoresDurableActiveSceneAndReleasesFiles) {
    namespace fs = std::filesystem;
    const auto package =
        fs::temp_directory_path() /
        ("creator-studio-workflow-real-" +
         std::to_string(QCoreApplication::applicationPid()) + ".creatorproj");
    std::error_code cleanupError;
    fs::remove_all(package, cleanupError);
    auto packages =
        std::make_shared<creator::project_store::ProjectPackageStore>();
    const auto created = packages->create(package, "Studio workflow");
    ASSERT_TRUE(created.hasValue()) << created.error().message();
    {
        StudioWorkflowController controller{
            [packages](const fs::path& root)
                -> Result<std::unique_ptr<IStudioStore>> {
                auto opened = packages->open(root);
                if (!opened.hasValue()) return opened.error();
                auto lease = opened.value().databaseIdentityLease;
                auto store = creator::project_store::SqliteStudioStore::open(
                    opened.value().databasePath,
                    opened.value().package.manifest.projectId,
                    [lease] { return lease->verifyCurrentIdentity(); });
                if (!store.hasValue()) return store.error();
                return std::unique_ptr<IStudioStore>{
                    new creator::project_store::SqliteStudioStore{
                        std::move(store).value()}};
            },
            std::make_unique<NoopReconciler>(),
            [next = std::uint64_t{0}]() mutable {
                return "real-studio-id-" + std::to_string(++next);
            }};
        const auto url = QUrl::fromLocalFile(
            QString::fromStdWString(package.wstring()));
        controller.openProject(url);
        ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
        ASSERT_EQ(controller.sceneModel()->rowCount(), 3);
        controller.switchScene(QStringLiteral("camera"));
        ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
        ASSERT_EQ(controller.activeSceneId(), QStringLiteral("camera"));
        controller.reopenProject();
        ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
        EXPECT_EQ(controller.activeSceneId(), QStringLiteral("camera"));
    }
    fs::remove_all(package, cleanupError);
    EXPECT_FALSE(cleanupError) << cleanupError.message();
    EXPECT_FALSE(fs::exists(package));
}

}  // namespace
