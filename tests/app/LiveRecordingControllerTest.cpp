#include "app/LiveRecordingController.h"

#include "app/IRecordingPersistence.h"
#include "core/AppError.h"
#include "core/Timebase.h"
#include "domain/RecordingSession.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

using creator::app::ILiveRecordingEngine;
using creator::app::IRecordingPersistence;
using creator::app::LiveRecordingCompletion;
using creator::app::LiveRecordingController;
using creator::app::LiveRecordingEngineSnapshot;
using creator::app::LiveRecordingOperationState;
using creator::app::LiveRecordingStart;
using creator::core::AppError;
using creator::core::ErrorCode;
using creator::core::Result;
using creator::core::TimestampNs;
using creator::domain::RecordingSession;

class PersistenceFake final : public IRecordingPersistence {
public:
    void begin(const creator::domain::SessionId& sessionId, TimestampNs startedAt,
               Completion completion) override {
        ++beginCalls;
        lastSessionId = sessionId.value();
        lastStartedAt = startedAt;
        pendingBegin = std::move(completion);
    }
    void complete(const RecordingSession& session, Completion completion) override {
        ++completeCalls;
        completedSession = session;
        pendingComplete = std::move(completion);
    }
    void abort(const creator::domain::SessionId& sessionId, std::string reason,
               Completion completion) override {
        ++abortCalls;
        lastSessionId = sessionId.value();
        abortReason = std::move(reason);
        pendingAbort = std::move(completion);
    }

    void finishBegin(Result<void> result = creator::core::ok()) {
        auto callback = std::move(*pendingBegin);
        pendingBegin.reset();
        callback(std::move(result));
    }
    void finishComplete(Result<void> result = creator::core::ok()) {
        auto callback = std::move(*pendingComplete);
        pendingComplete.reset();
        callback(std::move(result));
    }
    void finishAbort(Result<void> result = creator::core::ok()) {
        auto callback = std::move(*pendingAbort);
        pendingAbort.reset();
        callback(std::move(result));
    }

    int beginCalls{0};
    int completeCalls{0};
    int abortCalls{0};
    std::string lastSessionId;
    TimestampNs lastStartedAt{};
    std::optional<RecordingSession> completedSession;
    std::string abortReason;
    std::optional<Completion> pendingBegin;
    std::optional<Completion> pendingComplete;
    std::optional<Completion> pendingAbort;
};

class LiveEngineFake final : public ILiveRecordingEngine {
public:
    [[nodiscard]] bool available() const noexcept override { return isAvailable; }
    [[nodiscard]] std::string unavailableReason() const override { return reason; }
    [[nodiscard]] Result<void> start(LiveRecordingStart start,
                                     Completion completion) override {
        ++startCalls;
        lastStart = start;
        if (startError) return *startError;
        RecordingSession session{start.sessionId};
        auto started = session.start(start.startedAt);
        if (!started.hasValue()) return started.error();
        session_ = std::move(session);
        completion_ = std::move(completion);
        return creator::core::ok();
    }
    void stopAsync(TimestampNs stoppedAt) override {
        ++stopCalls;
        stopAt = stoppedAt;
    }
    [[nodiscard]] LiveRecordingEngineSnapshot snapshot() const override {
        return liveSnapshot;
    }

    void finish(TimestampNs stoppedAt, std::optional<AppError> terminal = std::nullopt) {
        ASSERT_TRUE(session_.has_value());
        ASSERT_TRUE(session_->stop(stoppedAt).hasValue());
        auto callback = std::move(completion_);
        callback(LiveRecordingCompletion{.session = std::move(*session_),
                                         .trackCount = liveSnapshot.trackCount,
                                         .segmentsPublished = liveSnapshot.segmentsPublished,
                                         .videoFramesDropped = liveSnapshot.videoFramesDropped,
                                         .terminalError = std::move(terminal)});
        session_.reset();
    }

    bool isAvailable{true};
    std::string reason{"FFmpeg recording is unavailable"};
    int startCalls{0};
    int stopCalls{0};
    std::optional<AppError> startError;
    std::optional<LiveRecordingStart> lastStart;
    std::optional<RecordingSession> session_;
    Completion completion_;
    TimestampNs stopAt{};
    LiveRecordingEngineSnapshot liveSnapshot;
};

struct Fixture final {
    Fixture() {
        auto engine = std::make_unique<LiveEngineFake>();
        engineRaw = engine.get();
        controller = std::make_unique<LiveRecordingController>(
            std::move(engine), &persistence,
            [] { return std::optional<std::filesystem::path>{"D:/take.cstudio"}; },
            [this] { return now; });
    }

    PersistenceFake persistence;
    LiveEngineFake* engineRaw{};
    TimestampNs now{std::chrono::seconds{10}};
    std::unique_ptr<LiveRecordingController> controller;
};

TEST(LiveRecordingControllerTest, RejectsUnavailableEngineBeforeCreatingDatabaseSession) {
    Fixture fixture;
    fixture.engineRaw->isAvailable = false;
    fixture.engineRaw->reason = "Audited FFmpeg runtime is not installed";

    fixture.controller->startRecording();

    EXPECT_EQ(fixture.persistence.beginCalls, 0);
    EXPECT_EQ(fixture.engineRaw->startCalls, 0);
    EXPECT_EQ(fixture.controller->statusMessage(),
              QStringLiteral("Audited FFmpeg runtime is not installed"));
}

TEST(LiveRecordingControllerTest, PersistsBeginBeforeEngineStartAndCompleteAfterStop) {
    Fixture fixture;

    fixture.controller->startRecording();
    ASSERT_EQ(fixture.controller->operationState(),
              LiveRecordingOperationState::Preparing);
    ASSERT_EQ(fixture.persistence.beginCalls, 1);
    EXPECT_EQ(fixture.engineRaw->startCalls, 0);
    fixture.persistence.finishBegin();

    ASSERT_EQ(fixture.controller->operationState(),
              LiveRecordingOperationState::Recording);
    ASSERT_EQ(fixture.engineRaw->startCalls, 1);
    EXPECT_EQ(fixture.engineRaw->lastStart->packagePath,
              std::filesystem::path{"D:/take.cstudio"});
    fixture.engineRaw->liveSnapshot = {.trackCount = 3,
                                       .queuedItems = 4,
                                       .videoFramesDropped = 2,
                                       .syncVideoFramesDropped = 6,
                                       .syncVideoFramesDuplicated = 7,
                                       .maximumAbsoluteDriftNanoseconds = 8'500'000,
                                       .audioCorrectionPpm = 125.0,
                                       .segmentsPublished = 5,
                                       .availableDiskBytes = 8ULL << 30U,
                                       .encoderName = "h264_videotoolbox"};
    fixture.now = TimestampNs{std::chrono::seconds{15}};
    fixture.controller->pollDiagnostics();
    EXPECT_EQ(fixture.controller->segmentCount(), 5);
    EXPECT_EQ(fixture.controller->trackCount(), 3);
    EXPECT_EQ(fixture.controller->syncDroppedFrames(), 6u);
    EXPECT_EQ(fixture.controller->duplicatedFrames(), 7u);
    EXPECT_DOUBLE_EQ(fixture.controller->maximumDriftMilliseconds(), 8.5);
    EXPECT_DOUBLE_EQ(fixture.controller->audioCorrectionPpm(), 125.0);
    EXPECT_EQ(fixture.controller->takeDuration(), QStringLiteral("00:00:05"));

    fixture.controller->stopRecording();
    ASSERT_EQ(fixture.controller->operationState(),
              LiveRecordingOperationState::Finalizing);
    ASSERT_EQ(fixture.engineRaw->stopCalls, 1);
    fixture.engineRaw->finish(fixture.now);
    ASSERT_EQ(fixture.persistence.completeCalls, 1);
    ASSERT_TRUE(fixture.persistence.completedSession.has_value());
    EXPECT_EQ(fixture.persistence.completedSession->segmentCount(), 0u);
    fixture.persistence.finishComplete();

    EXPECT_EQ(fixture.controller->operationState(), LiveRecordingOperationState::Idle);
    EXPECT_EQ(fixture.controller->statusMessage(), QStringLiteral("Stopped"));
    EXPECT_EQ(fixture.controller->segmentCount(), 5);
}

TEST(LiveRecordingControllerTest, AbortsDatabaseSessionWhenEngineCannotStart) {
    Fixture fixture;
    fixture.engineRaw->startError =
        AppError{ErrorCode::InvalidState, "Start at least one capture source"};

    fixture.controller->startRecording();
    fixture.persistence.finishBegin();

    ASSERT_EQ(fixture.persistence.abortCalls, 1);
    EXPECT_EQ(fixture.persistence.abortReason, "Start at least one capture source");
    fixture.persistence.finishAbort();
    EXPECT_EQ(fixture.controller->operationState(), LiveRecordingOperationState::Idle);
    EXPECT_EQ(fixture.controller->statusMessage(),
              QStringLiteral("Start at least one capture source"));
}

TEST(LiveRecordingControllerTest, CompletesSafeSegmentsThenShowsTerminalCaptureError) {
    Fixture fixture;
    fixture.controller->startRecording();
    fixture.persistence.finishBegin();
    fixture.engineRaw->liveSnapshot.segmentsPublished = 2;

    fixture.engineRaw->finish(
        TimestampNs{std::chrono::seconds{12}},
        AppError{ErrorCode::NotFound, "microphone disconnected"});

    ASSERT_EQ(fixture.controller->operationState(),
              LiveRecordingOperationState::Finalizing);
    ASSERT_EQ(fixture.persistence.completeCalls, 1);
    fixture.persistence.finishComplete();
    EXPECT_EQ(fixture.controller->operationState(), LiveRecordingOperationState::Idle);
    EXPECT_EQ(fixture.controller->statusMessage(),
              QStringLiteral("microphone disconnected"));
}

}  // namespace
