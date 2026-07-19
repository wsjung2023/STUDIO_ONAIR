#include "app/StudioRecordingBinding.h"

#include "core/AppError.h"
#include "domain/RecordingSession.h"

#include <gtest/gtest.h>

#include <QUrl>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace {

using creator::app::ILiveRecordingEngine;
using creator::app::IRecordingPersistence;
using creator::app::LiveRecordingCompletion;
using creator::app::LiveRecordingController;
using creator::app::LiveRecordingEngineSnapshot;
using creator::app::LiveRecordingStart;
using creator::app::StudioRecordingBinding;
using creator::core::AppError;
using creator::core::ErrorCode;
using creator::core::Result;
using creator::core::TimestampNs;
using creator::domain::RecordingSession;

class OrderedPersistence final : public IRecordingPersistence {
public:
    explicit OrderedPersistence(std::vector<std::string>& order) : order_(order) {}

    void begin(const creator::domain::SessionId&, TimestampNs,
               Completion completion) override {
        order_.push_back("begin");
        completion(creator::core::ok());
    }

    void complete(const RecordingSession&, Completion completion) override {
        order_.push_back("project complete");
        completion(creator::core::ok());
    }

    void abort(const creator::domain::SessionId&, std::string,
               Completion completion) override {
        order_.push_back("project abort");
        completion(creator::core::ok());
    }

private:
    std::vector<std::string>& order_;
};

class OrderedEngine final : public ILiveRecordingEngine {
public:
    explicit OrderedEngine(std::vector<std::string>& order) : order_(order) {}

    [[nodiscard]] bool available() const noexcept override { return true; }
    [[nodiscard]] std::string unavailableReason() const override { return {}; }

    [[nodiscard]] Result<void> start(LiveRecordingStart start,
                                     Completion completion) override {
        order_.push_back("engine start");
        startedSources = start.sources;
        RecordingSession session{start.sessionId};
        auto started = session.start(start.startedAt);
        if (!started.hasValue()) return started.error();
        session_ = std::move(session);
        completion_ = std::move(completion);
        return creator::core::ok();
    }

    [[nodiscard]] Result<std::vector<creator::app::LiveCaptureSource>>
    sourceSnapshot() const override {
        return availableSources;
    }

    void stopAsync(TimestampNs stoppedAt) override {
        order_.push_back("engine complete");
        ASSERT_TRUE(session_.has_value());
        ASSERT_TRUE(session_->stop(stoppedAt).hasValue());
        auto completion = std::move(completion_);
        completion(LiveRecordingCompletion{.session = std::move(*session_)});
        session_.reset();
    }

    [[nodiscard]] LiveRecordingEngineSnapshot snapshot() const override {
        return {};
    }

private:
    std::vector<std::string>& order_;
    std::optional<RecordingSession> session_;
    Completion completion_;
public:
    std::vector<creator::app::LiveCaptureSource> availableSources{
        {.sourceId = creator::domain::SourceId::create(
                         "screen-source-42").value(),
         .role = creator::recorder::TrackRole::Screen}};
    std::vector<creator::app::LiveCaptureSource> startedSources;
};

struct BindingFixture final {
    BindingFixture() : persistence(order) {
        auto engine = std::make_unique<OrderedEngine>(order);
        engineRaw = engine.get();
        recording = std::make_unique<LiveRecordingController>(
            std::move(engine), &persistence,
            [] { return std::optional<std::filesystem::path>{"D:/project.cstudio"}; },
            [this] { return now; });

        StudioRecordingBinding::WorkflowPorts ports{
            .prepare = [this](
                           creator::domain::SessionId,
                           std::vector<creator::project_store::RecordingSourceRole>
                               sources,
                           TimestampNs,
                           StudioRecordingBinding::Completion completion) {
                order.push_back("studio prepare");
                preparedSources = std::move(sources);
                if (deferPreparation) {
                    pendingPreparation = std::move(completion);
                } else {
                    completion(creator::core::ok());
                }
            },
            .abort = [this](StudioRecordingBinding::Completion completion) {
                order.push_back("studio abort");
                completion(creator::core::ok());
            },
            .complete = [this](StudioRecordingBinding::Completion completion) {
                order.push_back("reconcile");
                if (deferReconciliation) {
                    pendingReconciliation = std::move(completion);
                } else if (reconcileSucceeds) {
                    completion(creator::core::ok());
                } else {
                    completion(AppError{ErrorCode::IoFailure,
                                        "injected reconciliation failure"});
                }
            },
            .open = [this](QUrl,
                           StudioRecordingBinding::Completion completion) {
                order.push_back("studio reopen");
                if (openSucceeds) {
                    completion(creator::core::ok());
                } else {
                    completion(AppError{ErrorCode::InvalidState,
                                        "Studio operation is already in progress"});
                }
            }};
        binding = std::make_unique<StudioRecordingBinding>(
            *recording, std::move(ports),
            [this] { return projectUrl; });
        QObject::connect(binding.get(),
                         &StudioRecordingBinding::timelineReconciled,
                         binding.get(), [this](const QUrl&) {
                             order.push_back("editor reopen");
                         });
    }

    std::vector<std::string> order;
    OrderedPersistence persistence;
    OrderedEngine* engineRaw{};
    TimestampNs now{std::chrono::seconds{10}};
    QUrl projectUrl{QUrl::fromLocalFile(QStringLiteral("D:/project.cstudio"))};
    bool reconcileSucceeds{true};
    bool deferReconciliation{false};
    bool openSucceeds{true};
    bool deferPreparation{false};
    std::optional<StudioRecordingBinding::Completion> pendingPreparation;
    std::optional<StudioRecordingBinding::Completion> pendingReconciliation;
    std::vector<creator::project_store::RecordingSourceRole> preparedSources;
    std::unique_ptr<LiveRecordingController> recording;
    std::unique_ptr<StudioRecordingBinding> binding;
};

TEST(StudioRecordingBindingTest,
     OrdersDurablePreparationCaptureCompletionReconcileAndEditorRefresh) {
    BindingFixture fixture;

    fixture.recording->startRecording();
    ASSERT_TRUE(fixture.recording->isRecording());
    fixture.now = TimestampNs{std::chrono::seconds{12}};
    fixture.recording->stopRecording();

    EXPECT_EQ(fixture.order,
              (std::vector<std::string>{
                  "begin", "studio prepare", "engine start",
                  "engine complete", "project complete", "reconcile",
                  "editor reopen"}));
    ASSERT_EQ(fixture.preparedSources.size(), 1U);
    EXPECT_EQ(fixture.preparedSources.front().sourceId.value(),
              "screen-source-42");
    EXPECT_EQ(fixture.preparedSources.front().role,
              creator::domain::StudioSourceRole::Screen);
}

TEST(StudioRecordingBindingTest,
     FailedReconcileLeavesEditorUntouchedAndProjectReopenRetries) {
    BindingFixture fixture;
    fixture.reconcileSucceeds = false;

    fixture.recording->startRecording();
    fixture.now = TimestampNs{std::chrono::seconds{12}};
    fixture.recording->stopRecording();

    EXPECT_EQ(std::ranges::count(fixture.order, "editor reopen"), 0);
    EXPECT_EQ(std::ranges::count(fixture.order, "reconcile"), 1);

    fixture.binding->openProject(fixture.projectUrl);

    EXPECT_EQ(std::ranges::count(fixture.order, "studio reopen"), 1);
    EXPECT_EQ(std::ranges::count(fixture.order, "editor reopen"), 1);
}

TEST(StudioRecordingBindingTest,
     EngineUsesTheExactConcreteSourceSnapshotPersistedByStudio) {
    BindingFixture fixture;
    fixture.deferPreparation = true;

    fixture.recording->startRecording();
    ASSERT_TRUE(fixture.pendingPreparation.has_value());
    fixture.engineRaw->availableSources = {
        {.sourceId = creator::domain::SourceId::create(
                         "camera-source-after-click").value(),
         .role = creator::recorder::TrackRole::Camera}};
    auto prepared = std::move(*fixture.pendingPreparation);
    fixture.pendingPreparation.reset();
    prepared(creator::core::ok());

    ASSERT_TRUE(fixture.recording->isRecording());
    ASSERT_EQ(fixture.engineRaw->startedSources.size(), 1U);
    EXPECT_EQ(fixture.engineRaw->startedSources.front().sourceId.value(),
              "screen-source-42");
    ASSERT_EQ(fixture.preparedSources.size(), 1U);
    EXPECT_EQ(fixture.preparedSources.front().sourceId,
              fixture.engineRaw->startedSources.front().sourceId);
    fixture.now = TimestampNs{std::chrono::seconds{12}};
    fixture.recording->stopRecording();
}

TEST(StudioRecordingBindingTest,
     DestructionDuringRecordingClearsPreparationForTheNextTake) {
    BindingFixture fixture;
    fixture.recording->startRecording();
    ASSERT_TRUE(fixture.recording->isRecording());
    fixture.binding.reset();
    fixture.now = TimestampNs{std::chrono::seconds{12}};
    fixture.recording->stopRecording();
    ASSERT_FALSE(fixture.recording->isRecording());

    fixture.now = TimestampNs{std::chrono::seconds{20}};
    fixture.recording->startRecording();

    EXPECT_TRUE(fixture.recording->isRecording());
    EXPECT_EQ(std::ranges::count(fixture.order, "studio prepare"), 1);
    fixture.now = TimestampNs{std::chrono::seconds{22}};
    fixture.recording->stopRecording();
}

TEST(StudioRecordingBindingTest,
     RejectedOpenDoesNotCancelPendingSuccessfulReconciliation) {
    BindingFixture fixture;
    fixture.deferReconciliation = true;
    fixture.recording->startRecording();
    fixture.now = TimestampNs{std::chrono::seconds{12}};
    fixture.recording->stopRecording();
    ASSERT_TRUE(fixture.pendingReconciliation.has_value());

    fixture.openSucceeds = false;
    fixture.binding->openProject(QUrl::fromLocalFile(
        QStringLiteral("D:/other-project.cstudio")));
    EXPECT_EQ(std::ranges::count(fixture.order, "editor reopen"), 0);

    auto reconciled = std::move(*fixture.pendingReconciliation);
    fixture.pendingReconciliation.reset();
    reconciled(creator::core::ok());

    EXPECT_EQ(std::ranges::count(fixture.order, "editor reopen"), 1);
}

}  // namespace
