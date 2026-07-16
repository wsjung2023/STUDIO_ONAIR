#include "app/StudioController.h"

#include "core/AppError.h"
#include "domain/Identifiers.h"
#include "fakes/FakeCaptureSource.h"
#include "fakes/FakeRecorder.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <gtest/gtest.h>

#include <memory>

namespace {

using creator::app::StudioController;
using creator::core::AppError;
using creator::core::ErrorCode;
using creator::domain::SourceId;
using creator::fakes::FakeCaptureSource;
using creator::fakes::FakeRecorder;

std::unique_ptr<StudioController> makeController() {
    return std::make_unique<StudioController>(
        std::make_unique<FakeCaptureSource>(SourceId::create("screen-1").value(), "Fake Screen"),
        std::make_unique<FakeRecorder>());
}

/// Q_PROPERTY notification and QSignalSpy need a QCoreApplication to exist.
class StudioControllerTest : public ::testing::Test {
protected:
    void SetUp() override { controller_ = makeController(); }

    std::unique_ptr<StudioController> controller_;
};

TEST_F(StudioControllerTest, StartsIdle) {
    EXPECT_FALSE(controller_->isRecording());
    EXPECT_EQ(controller_->segmentCount(), 0);
}

TEST_F(StudioControllerTest, RecordStartsASession) {
    QSignalSpy spy{controller_.get(), &StudioController::recordingChanged};

    controller_->startRecording();

    EXPECT_TRUE(controller_->isRecording());
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(StudioControllerTest, StopEndsTheSession) {
    controller_->startRecording();

    controller_->stopRecording();

    EXPECT_FALSE(controller_->isRecording());
}

TEST_F(StudioControllerTest, StopReportsSegmentsAndDuration) {
    controller_->startRecording();

    // 6 seconds of 60fps frames at the 2s default segment length = 3 segments.
    for (int i = 0; i < 60 * 6; ++i) {
        controller_->onCaptureTick();
    }
    controller_->stopRecording();

    EXPECT_EQ(controller_->segmentCount(), 3);
    EXPECT_EQ(controller_->takeDuration(), QStringLiteral("00:00:06"));
}

TEST_F(StudioControllerTest, FormatsLongDurations) {
    controller_->startRecording();

    // One hour, one minute, one second of frames.
    for (int i = 0; i < 60 * 3661; ++i) {
        controller_->onCaptureTick();
    }
    controller_->stopRecording();

    EXPECT_EQ(controller_->takeDuration(), QStringLiteral("01:01:01"));
}

TEST_F(StudioControllerTest, EmitsTakeSummaryOnStop) {
    QSignalSpy spy{controller_.get(), &StudioController::takeSummaryChanged};
    controller_->startRecording();
    for (int i = 0; i < 60 * 3; ++i) {
        controller_->onCaptureTick();
    }

    controller_->stopRecording();

    EXPECT_GE(spy.count(), 1);
}

TEST_F(StudioControllerTest, IgnoresTickWhileIdle) {
    // The timer keeps running between takes; a stray tick must not crash or
    // invent frames.
    controller_->onCaptureTick();

    EXPECT_FALSE(controller_->isRecording());
    EXPECT_EQ(controller_->segmentCount(), 0);
}

TEST_F(StudioControllerTest, IgnoresDoubleRecord) {
    controller_->startRecording();
    QSignalSpy spy{controller_.get(), &StudioController::recordingChanged};

    controller_->startRecording();

    EXPECT_TRUE(controller_->isRecording());
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(StudioControllerTest, IgnoresStopWhileIdle) {
    controller_->stopRecording();

    EXPECT_FALSE(controller_->isRecording());
}

TEST_F(StudioControllerTest, SurfacesFailureInStatusMessage) {
    // A recording failure must reach the UI with its OWN message - not just
    // some non-empty text, and not the unrelated "Not recording" idle guard -
    // or CLAUDE.md 9's "expose device errors" requirement is unverifiable.
    auto source = std::make_unique<FakeCaptureSource>(SourceId::create("screen-1").value(),
                                                       "Fake Screen");
    source->failNextStart(AppError{ErrorCode::Unknown, "injected start failure"});
    StudioController controller{std::move(source), std::make_unique<FakeRecorder>()};

    controller.startRecording();

    EXPECT_EQ(controller.statusMessage(), QStringLiteral("injected start failure"));
}

TEST_F(StudioControllerTest, FailedStartStopsTheSourceAndLeavesControllerIdle) {
    // A source that fails to start must still be released (ICaptureSource::
    // stop()'s contract says it is safe to call on a source that never
    // started, precisely so this unwind can rely on it unconditionally).
    // Injecting a distinct failNextStop error and asserting THAT message (not
    // the start failure's) reaches statusMessage() is what proves stop() was
    // actually invoked during the unwind - the source's own state after a
    // failed start is otherwise indistinguishable from "never touched".
    auto source = std::make_unique<FakeCaptureSource>(SourceId::create("screen-1").value(),
                                                       "Fake Screen");
    source->failNextStart(AppError{ErrorCode::Unknown, "start denied"});
    source->failNextStop(AppError{ErrorCode::Unknown, "stop also failed"});
    StudioController controller{std::move(source), std::make_unique<FakeRecorder>()};

    controller.startRecording();

    EXPECT_FALSE(controller.isRecording());
    EXPECT_EQ(controller.statusMessage(), QStringLiteral("stop also failed"));
}

TEST_F(StudioControllerTest, ClearsPreviousTakeOnRestart) {
    controller_->startRecording();
    for (int i = 0; i < 60 * 4; ++i) {
        controller_->onCaptureTick();
    }
    controller_->stopRecording();
    ASSERT_EQ(controller_->segmentCount(), 2);

    controller_->startRecording();

    EXPECT_EQ(controller_->segmentCount(), 0);
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app{argc, argv};
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
