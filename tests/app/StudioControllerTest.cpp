#include "app/StudioController.h"

#include "domain/Identifiers.h"
#include "fakes/FakeCaptureSource.h"
#include "fakes/FakeRecorder.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <gtest/gtest.h>

#include <memory>

namespace {

using creator::app::StudioController;
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
    // A recording failure must reach the UI, not just the log (CLAUDE.md 9).
    controller_->stopRecording();

    EXPECT_FALSE(controller_->statusMessage().isEmpty());
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
