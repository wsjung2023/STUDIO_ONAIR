#include "app/ExportController.h"

#include "core/Uuid.h"
#include "domain/Timeline.h"

#include <QSignalSpy>
#include <QThread>
#include <QUrl>

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>

namespace {

using namespace creator;

struct EngineState final {
    std::atomic<Qt::HANDLE> renderThread{};
    std::atomic<bool> cancelled{};
    std::atomic<bool> jobDestroyed{};
    bool waitsForCancellation{};
    std::atomic<std::int64_t> renderedRevision{};
    std::atomic<std::uint32_t> renderedWidth{};
};

class ControllerRenderJob final : public edit_engine::IRenderJob {
public:
    explicit ControllerRenderJob(std::shared_ptr<EngineState> state)
        : state_(std::move(state)) {}
    ~ControllerRenderJob() override { state_->jobDestroyed = true; }

    core::Result<edit_engine::RenderProgress> progress() const override {
        const auto total = core::DurationNs{1'000};
        if (state_->cancelled) {
            return edit_engine::RenderProgress::create(
                edit_engine::RenderJobState::Cancelled, 0.25,
                core::TimestampNs{core::DurationNs{250}}, total);
        }
        if (state_->waitsForCancellation) {
            return edit_engine::RenderProgress::create(
                edit_engine::RenderJobState::Running, 0.25,
                core::TimestampNs{core::DurationNs{250}}, total);
        }
        return edit_engine::RenderProgress::create(
            edit_engine::RenderJobState::Completed, 1.0,
            core::TimestampNs{total}, total);
    }

    core::Result<void> cancel() override {
        state_->cancelled = true;
        return core::ok();
    }

private:
    std::shared_ptr<EngineState> state_;
};

class ControllerEditEngine final : public edit_engine::IEditEngine {
public:
    explicit ControllerEditEngine(std::shared_ptr<EngineState> state)
        : state_(std::move(state)) {}
    core::Result<void> load(const edit_engine::TimelineSnapshot&) override {
        return core::ok();
    }
    core::Result<void> update(const edit_engine::TimelineChangeSet&) override {
        return core::ok();
    }
    core::Result<void> play() override { return core::ok(); }
    core::Result<void> pause() override { return core::ok(); }
    core::Result<void> seek(core::TimestampNs) override { return core::ok(); }
    core::Result<edit_engine::PreviewFrame> requestFrame(
        core::TimestampNs) override {
        return core::AppError{core::ErrorCode::InvalidState, "unused"};
    }
    core::Result<std::unique_ptr<edit_engine::IRenderJob>> render(
        const edit_engine::RenderRequest& request) override {
        state_->renderThread = QThread::currentThreadId();
        state_->renderedRevision = request.snapshot().revision.value();
        state_->renderedWidth = request.preset().width();
        std::unique_ptr<edit_engine::IRenderJob> job =
            std::make_unique<ControllerRenderJob>(state_);
        return job;
    }

private:
    std::shared_ptr<EngineState> state_;
};

edit_engine::RenderRequest request(const std::filesystem::path& root) {
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("timeline").value(), "Main",
                        core::FrameRate::create(30, 1).value())
                        .value();
    const auto trackId = domain::TrackId::create("titles").value();
    EXPECT_TRUE(timeline.addTrack(domain::Track::create(
                                      trackId, domain::TrackKind::Title,
                                      "Titles", true, false)
                                      .value())
                    .hasValue());
    auto payload = domain::TitlePayload::create(
                       "Export", "Creator Sans", 0.5, 0.5,
                       domain::RgbaColor::parse("#ffffffff").value(),
                       domain::RgbaColor::parse("#00000000").value(),
                       domain::TextAlignment::Center)
                       .value();
    EXPECT_TRUE(timeline
                    .insertClip(
                        trackId,
                        domain::Clip::createTitle(
                            domain::ClipId::create("title").value(),
                            domain::TimeRange::create(
                                core::TimestampNs{}, core::DurationNs{1'000})
                                .value(),
                            true, std::move(payload), std::nullopt)
                            .value())
                    .hasValue());
    return edit_engine::RenderRequest::create(
               domain::ProjectId::create("project").value(),
               {std::move(timeline),
                domain::TimelineRevision::create(1).value()},
               root / "output.mp4",
               edit_engine::RenderPreset::h2641080p30().value(),
               edit_engine::RenderOverwritePolicy::FailIfExists)
        .value();
}

class ExportControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
                ("creator-studio-export-controller-" + core::generateUuidV4());
        ASSERT_TRUE(std::filesystem::create_directories(root_));
        state_ = std::make_shared<EngineState>();
    }
    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    std::filesystem::path root_;
    std::shared_ptr<EngineState> state_;
};

TEST_F(ExportControllerTest, RunsRenderOffUiThreadAndPublishesCompletion) {
    app::ExportController controller{
        std::make_unique<ControllerEditEngine>(state_)};
    controller.setRequest(request(root_));
    QSignalSpy finished{&controller, &app::ExportController::exportFinished};

    controller.startExport();

    ASSERT_TRUE(finished.wait(3000));
    EXPECT_NE(state_->renderThread.load(), QThread::currentThreadId());
    EXPECT_FALSE(controller.busy());
    EXPECT_DOUBLE_EQ(controller.progress(), 1.0);
    EXPECT_TRUE(finished.at(0).at(0).toBool());
}

TEST_F(ExportControllerTest, CancelAndDestructionDrainPendingJob) {
    state_->waitsForCancellation = true;
    {
        app::ExportController controller{
            std::make_unique<ControllerEditEngine>(state_)};
        controller.setRequest(request(root_));
        QSignalSpy finished{&controller, &app::ExportController::exportFinished};
        controller.startExport();
        while (state_->renderThread.load() == nullptr) QThread::yieldCurrentThread();
        controller.cancelExport();
        ASSERT_TRUE(finished.wait(3000));
        EXPECT_TRUE(state_->cancelled);
        EXPECT_FALSE(finished.at(0).at(0).toBool());
    }
    EXPECT_TRUE(state_->jobDestroyed);
}

TEST_F(ExportControllerTest, PreservesCancellationRequestedBeforeWorkerEntry) {
    state_->waitsForCancellation = true;
    app::ExportController controller{
        std::make_unique<ControllerEditEngine>(state_)};
    controller.setRequest(request(root_));
    QSignalSpy finished{&controller, &app::ExportController::exportFinished};

    controller.startExport();
    controller.cancelExport();

    ASSERT_TRUE(finished.wait(3000));
    EXPECT_TRUE(state_->cancelled);
    EXPECT_FALSE(finished.at(0).at(0).toBool());
}

TEST_F(ExportControllerTest, RejectsStartWithoutFrozenRequest) {
    app::ExportController controller{
        std::make_unique<ControllerEditEngine>(state_)};
    controller.startExport();
    EXPECT_FALSE(controller.busy());
    EXPECT_FALSE(controller.statusMessage().isEmpty());
}

TEST_F(ExportControllerTest, FreezesCurrentSourceWithSelectedProductPreset) {
    app::ExportController controller{
        std::make_unique<ControllerEditEngine>(state_)};
    auto source = request(root_);
    controller.setSource(source.projectId(), source.snapshot());
    QSignalSpy finished{&controller, &app::ExportController::exportFinished};

    controller.exportTo(QUrl::fromLocalFile(
                            QString::fromStdWString((root_ / "four-k.mp4").wstring())),
                        QStringLiteral("h264-2160p30"), false);

    ASSERT_TRUE(finished.wait(3000));
    EXPECT_EQ(state_->renderedRevision.load(), 1);
    EXPECT_EQ(state_->renderedWidth.load(), 3840U);
    EXPECT_TRUE(controller.ready());
}

TEST_F(ExportControllerTest, RejectsUnsupportedPresetWithoutStartingWorker) {
    app::ExportController controller{
        std::make_unique<ControllerEditEngine>(state_)};
    auto source = request(root_);
    controller.setSource(source.projectId(), source.snapshot());

    controller.exportTo(QUrl::fromLocalFile(
                            QString::fromStdWString((root_ / "bad.mp4").wstring())),
                        QStringLiteral("unknown"), false);

    EXPECT_FALSE(controller.busy());
    EXPECT_EQ(state_->renderThread.load(), nullptr);
    EXPECT_FALSE(controller.statusMessage().isEmpty());
}

TEST_F(ExportControllerTest, RepeatedPageCloseCancelsAndJoinsWorker) {
    for (int iteration = 0; iteration < 100; ++iteration) {
        auto state = std::make_shared<EngineState>();
        state->waitsForCancellation = true;
        {
            app::ExportController controller{
                std::make_unique<ControllerEditEngine>(state)};
            controller.setRequest(request(root_));
            controller.startExport();
            while (state->renderThread.load() == nullptr) {
                QThread::yieldCurrentThread();
            }
        }
        ASSERT_TRUE(state->cancelled) << "iteration " << iteration;
        ASSERT_TRUE(state->jobDestroyed) << "iteration " << iteration;
    }
}

}  // namespace
