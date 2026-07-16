#include "project_store/SqliteProjectDatabase.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "core/Utc.h"
#include "domain/Identifiers.h"
#include "domain/ProjectManifest.h"
#include "domain/Segment.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>

namespace {

namespace fs = std::filesystem;

using creator::core::ErrorCode;
using creator::core::TimestampNs;
using creator::core::Utc;
using creator::domain::ProjectId;
using creator::domain::ProjectManifest;
using creator::domain::SegmentInfo;
using creator::domain::SegmentStatus;
using creator::domain::SessionId;
using creator::domain::SourceId;
using creator::project_store::PersistedSessionState;
using creator::project_store::SqliteProjectDatabase;

Utc utc(std::string_view text) {
    return Utc::parseRfc3339(text).value();
}

ProjectManifest manifest() {
    return ProjectManifest{
        .schemaVersion = 1,
        .projectId = ProjectId::create("123e4567-e89b-42d3-a456-426614174000").value(),
        .name = "Recovery",
        .createdAt = utc("2026-07-16T09:30:00Z"),
        .updatedAt = utc("2026-07-16T09:30:00Z"),
        .canvas = {},
        .database = "project.db",
        .directories = {},
        .requiredFeatures = {},
    };
}

SegmentInfo segment(SegmentStatus status, std::uint64_t index) {
    return SegmentInfo{
        .index = index,
        .sourceId = SourceId::create("screen-1").value(),
        .startTime = TimestampNs{} + std::chrono::seconds{2 * index},
        .duration = std::chrono::seconds{2},
        .status = status,
        .relativePath = "media/screen-1/segment_" + std::to_string(index) + ".mkv",
    };
}

class RecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        packagePath_ = fs::temp_directory_path() /
                       ("cs_recovery_" + std::string{info->test_suite_name()} + "_" +
                        std::string{info->name()} + ".cstudio");
        std::error_code ec;
        fs::remove_all(packagePath_, ec);
        fs::create_directories(packagePath_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(packagePath_, ec);
    }

    SqliteProjectDatabase recordingDatabase(TimestampNs startedAt = TimestampNs{}) {
        auto created =
            SqliteProjectDatabase::create(packagePath_ / "project.db", manifest());
        EXPECT_TRUE(created.hasValue()) << created.error().message();
        auto database = std::move(created).value();
        EXPECT_TRUE(database.beginRecording(sessionId_, startedAt,
                                            utc("2026-07-16T10:00:00Z"))
                        .hasValue());
        return database;
    }

    SqliteProjectDatabase databaseWithRecordingReadyAndWriting() {
        auto database = recordingDatabase();
        const auto readyWriting = segment(SegmentStatus::Writing, 0);
        EXPECT_TRUE(database.beginSegment(sessionId_, readyWriting).hasValue());
        EXPECT_TRUE(database.markSegmentReady(sessionId_, segment(SegmentStatus::Ready, 0))
                        .hasValue());
        EXPECT_TRUE(database.beginSegment(sessionId_, segment(SegmentStatus::Writing, 1))
                        .hasValue());
        return database;
    }

    fs::path packagePath_;
    SessionId sessionId_{SessionId::create("session-1").value()};
};

TEST_F(RecoveryTest, ScanReturnsOnlyRecordingSessionsWithCounts) {
    auto database = databaseWithRecordingReadyAndWriting();
    const auto completed = SessionId::create("completed").value();
    ASSERT_TRUE(database.beginRecording(completed, TimestampNs{},
                                        utc("2026-07-16T10:30:00Z"))
                    .hasValue());
    ASSERT_TRUE(database.completeRecording(completed, TimestampNs{}, {},
                                           utc("2026-07-16T10:30:00Z"))
                    .hasValue());

    const auto candidates = database.scanRecovery(packagePath_, "강의");

    ASSERT_TRUE(candidates.hasValue());
    ASSERT_EQ(candidates.value().size(), 1u);
    EXPECT_EQ(candidates.value()[0].sessionId, sessionId_);
    EXPECT_EQ(candidates.value()[0].packagePath, packagePath_);
    EXPECT_EQ(candidates.value()[0].projectName, "강의");
    EXPECT_EQ(candidates.value()[0].readySegments, 1u);
    EXPECT_EQ(candidates.value()[0].writingSegments, 1u);
}

TEST_F(RecoveryTest, RecoverKeepsReadyAndFailsOnlyWriting) {
    auto database = databaseWithRecordingReadyAndWriting();

    const auto recovered = database.recover(sessionId_, utc("2026-07-16T11:00:00Z"));

    ASSERT_TRUE(recovered.hasValue()) << recovered.error().message();
    EXPECT_EQ(recovered.value().stoppedAt, TimestampNs{} + std::chrono::seconds{2});
    EXPECT_EQ(recovered.value().readySegments, 1u);
    EXPECT_EQ(recovered.value().failedSegments, 1u);
    EXPECT_EQ(database.session(sessionId_).value().state, PersistedSessionState::Recovered);
    EXPECT_TRUE(database.scanRecovery(packagePath_, "Recovery").value().empty());
}

TEST_F(RecoveryTest, RecoverIsIdempotent) {
    auto database = databaseWithRecordingReadyAndWriting();

    const auto first = database.recover(sessionId_, utc("2026-07-16T11:00:00Z"));
    const auto second = database.recover(sessionId_, utc("2026-07-16T11:05:00Z"));

    ASSERT_TRUE(first.hasValue());
    ASSERT_TRUE(second.hasValue());
    EXPECT_EQ(second.value().stoppedAt, first.value().stoppedAt);
    EXPECT_EQ(second.value().readySegments, first.value().readySegments);
    EXPECT_EQ(second.value().failedSegments, first.value().failedSegments);
}

TEST_F(RecoveryTest, RecoverWithoutReadySegmentStopsAtSessionStart) {
    const auto startedAt = TimestampNs{} + std::chrono::seconds{5};
    auto database = recordingDatabase(startedAt);
    ASSERT_TRUE(database.beginSegment(sessionId_, segment(SegmentStatus::Writing, 1))
                    .hasValue());

    const auto recovered = database.recover(sessionId_, utc("2026-07-16T11:00:00Z"));

    ASSERT_TRUE(recovered.hasValue());
    EXPECT_EQ(recovered.value().stoppedAt, startedAt);
    EXPECT_EQ(recovered.value().readySegments, 0u);
    EXPECT_EQ(recovered.value().failedSegments, 1u);
}

TEST_F(RecoveryTest, RejectsOverflowingSegmentEndWithoutChangingSession) {
    auto database = recordingDatabase();
    auto writing = segment(SegmentStatus::Writing, 0);
    writing.startTime = TimestampNs{creator::core::DurationNs{
        std::numeric_limits<std::int64_t>::max() - 1}};
    writing.duration = std::chrono::nanoseconds{2};
    ASSERT_TRUE(database.beginSegment(sessionId_, writing).hasValue());
    auto ready = writing;
    ready.status = SegmentStatus::Ready;
    ASSERT_TRUE(database.markSegmentReady(sessionId_, ready).hasValue());

    const auto recovered = database.recover(sessionId_, utc("2026-07-16T11:00:00Z"));

    ASSERT_FALSE(recovered.hasValue());
    EXPECT_EQ(recovered.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(database.session(sessionId_).value().state, PersistedSessionState::Recording);
}

}  // namespace
