#include "project_store/SqliteProjectDatabase.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "core/Utc.h"
#include "domain/Identifiers.h"
#include "domain/ProjectManifest.h"
#include "domain/Segment.h"
#include "project_store/internal/SqliteConnection.h"

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
using creator::project_store::SqliteProjectDatabase;
using creator::project_store::internal::SqliteConnection;

Utc utc(std::string_view text) {
    return Utc::parseRfc3339(text).value();
}

ProjectManifest manifest() {
    return ProjectManifest{
        .schemaVersion = 1,
        .projectId = ProjectId::create("123e4567-e89b-42d3-a456-426614174000").value(),
        .name = "Segments",
        .createdAt = utc("2026-07-16T09:30:00Z"),
        .updatedAt = utc("2026-07-16T09:30:00Z"),
        .canvas = {},
        .database = "project.db",
        .directories = {},
        .requiredFeatures = {},
    };
}

SegmentInfo segment(SegmentStatus status, std::uint64_t index = 0) {
    return SegmentInfo{
        .index = index,
        .sourceId = SourceId::create("screen-1").value(),
        .startTime = TimestampNs{} + std::chrono::seconds{2 * index},
        .duration = std::chrono::seconds{2},
        .status = status,
        .relativePath = "media/screen-1/segment_00000" + std::to_string(index) + ".mkv",
    };
}

class SegmentPersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ = fs::temp_directory_path() /
                     ("cs_segment_" + std::string{info->test_suite_name()} + "_" +
                      std::string{info->name()});
        std::error_code ec;
        fs::remove_all(directory_, ec);
        fs::create_directories(directory_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(directory_, ec);
    }

    SqliteProjectDatabase activeDatabase() {
        auto created = SqliteProjectDatabase::create(directory_ / "project.db", manifest());
        EXPECT_TRUE(created.hasValue()) << created.error().message();
        auto database = std::move(created).value();
        EXPECT_TRUE(database.beginRecording(sessionId_, TimestampNs{},
                                            utc("2026-07-16T10:00:00Z"))
                        .hasValue());
        return database;
    }

    fs::path directory_;
    SessionId sessionId_{SessionId::create("session-1").value()};
};

TEST_F(SegmentPersistenceTest, AllowsWritingToReadyAndRejectsTerminalRewrite) {
    auto database = activeDatabase();
    ASSERT_TRUE(database.beginSegment(sessionId_, segment(SegmentStatus::Writing)).hasValue());
    ASSERT_TRUE(database.markSegmentReady(sessionId_, segment(SegmentStatus::Ready)).hasValue());
    EXPECT_TRUE(database.markSegmentReady(sessionId_, segment(SegmentStatus::Ready)).hasValue());

    auto changed = segment(SegmentStatus::Ready);
    changed.duration = std::chrono::seconds{3};
    const auto rewrite = database.markSegmentReady(sessionId_, changed);
    ASSERT_FALSE(rewrite.hasValue());
    EXPECT_EQ(rewrite.error().code(), ErrorCode::InvalidState);
}

TEST_F(SegmentPersistenceTest, RejectsAbsoluteAndEscapingPaths) {
    auto database = activeDatabase();
    for (const std::string path : {"C:/outside.mkv", "/outside.mkv", "../outside.mkv",
                                   "media/../../x.mkv", "\\\\server\\share\\x.mkv"}) {
        auto value = segment(SegmentStatus::Writing);
        value.relativePath = path;
        const auto result = database.beginSegment(sessionId_, value);
        EXPECT_FALSE(result.hasValue()) << path;
        if (!result.hasValue()) {
            EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument) << path;
        }
    }
}

TEST_F(SegmentPersistenceTest, AcceptsUnicodePathAndRejectsMalformedUtf8WithoutThrowing) {
    auto database = activeDatabase();
    auto unicode = segment(SegmentStatus::Writing);
    unicode.relativePath = "media/강의/화면_01.mkv";
    EXPECT_TRUE(database.beginSegment(sessionId_, unicode).hasValue());

    auto malformed = segment(SegmentStatus::Writing, 1);
    malformed.relativePath = std::string{"media/bad_"} + '\x80' + ".mkv";
    EXPECT_NO_THROW({
        const auto result = database.beginSegment(sessionId_, malformed);
        EXPECT_FALSE(result.hasValue());
    });
}

TEST_F(SegmentPersistenceTest, BeginAndFailureTransitionsAreIdempotent) {
    auto database = activeDatabase();
    const auto writing = segment(SegmentStatus::Writing);
    ASSERT_TRUE(database.beginSegment(sessionId_, writing).hasValue());
    EXPECT_TRUE(database.beginSegment(sessionId_, writing).hasValue());
    ASSERT_TRUE(database.markSegmentFailed(sessionId_, writing.sourceId, writing.index).hasValue());
    EXPECT_TRUE(database.markSegmentFailed(sessionId_, writing.sourceId, writing.index).hasValue());

    const auto ready = database.markSegmentReady(sessionId_, segment(SegmentStatus::Ready));
    ASSERT_FALSE(ready.hasValue());
    EXPECT_EQ(ready.error().code(), ErrorCode::InvalidState);
}

TEST_F(SegmentPersistenceTest, ExactTerminalRetriesRemainIdempotentAfterSessionEnds) {
    auto database = activeDatabase();
    const auto readyWriting = segment(SegmentStatus::Writing, 0);
    const auto ready = segment(SegmentStatus::Ready, 0);
    const auto failed = segment(SegmentStatus::Writing, 1);
    ASSERT_TRUE(database.beginSegment(sessionId_, readyWriting).hasValue());
    ASSERT_TRUE(database.markSegmentReady(sessionId_, ready).hasValue());
    ASSERT_TRUE(database.beginSegment(sessionId_, failed).hasValue());
    ASSERT_TRUE(database.markSegmentFailed(sessionId_, failed.sourceId, failed.index).hasValue());
    ASSERT_TRUE(database.abortRecording(sessionId_, "cancelled",
                                        utc("2026-07-16T10:00:01Z"))
                    .hasValue());

    EXPECT_TRUE(database.markSegmentReady(sessionId_, ready).hasValue());
    EXPECT_TRUE(database.markSegmentFailed(sessionId_, failed.sourceId, failed.index).hasValue());
}

TEST_F(SegmentPersistenceTest, CompleteRecordingStoresReadySegmentsAtomically) {
    auto database = activeDatabase();
    const auto ready = segment(SegmentStatus::Ready);

    ASSERT_TRUE(database.completeRecording(sessionId_, TimestampNs{} + std::chrono::seconds{2},
                                           {ready}, utc("2026-07-16T10:00:02Z"))
                    .hasValue());

    auto rawResult = SqliteConnection::open(directory_ / "project.db");
    ASSERT_TRUE(rawResult.hasValue());
    auto raw = std::move(rawResult).value();
    EXPECT_EQ(raw.scalarText("SELECT status FROM segments").value(), "READY");
    EXPECT_EQ(raw.scalarInt64("SELECT duration_ns FROM segments").value(),
              std::chrono::duration_cast<std::chrono::nanoseconds>(ready.duration).count());
}

TEST_F(SegmentPersistenceTest, CompleteRecordingRollsBackAllSegmentsOnConflict) {
    auto database = activeDatabase();
    const auto failed = segment(SegmentStatus::Writing, 0);
    ASSERT_TRUE(database.beginSegment(sessionId_, failed).hasValue());
    ASSERT_TRUE(database.markSegmentFailed(sessionId_, failed.sourceId, failed.index).hasValue());

    const auto result = database.completeRecording(
        sessionId_, TimestampNs{} + std::chrono::seconds{4},
        {segment(SegmentStatus::Ready, 1), segment(SegmentStatus::Ready, 0)},
        utc("2026-07-16T10:00:04Z"));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
    EXPECT_EQ(database.session(sessionId_).value().state,
              creator::project_store::PersistedSessionState::Recording);
    auto rawResult = SqliteConnection::open(directory_ / "project.db");
    ASSERT_TRUE(rawResult.hasValue());
    auto raw = std::move(rawResult).value();
    EXPECT_EQ(raw.scalarInt64("SELECT count(*) FROM segments WHERE segment_index=1").value(), 0);
}

TEST_F(SegmentPersistenceTest, RejectsInvalidSegmentStateDurationAndIndex) {
    auto database = activeDatabase();
    const auto readyWithoutWriting =
        database.beginSegment(sessionId_, segment(SegmentStatus::Ready));
    ASSERT_FALSE(readyWithoutWriting.hasValue());
    EXPECT_EQ(readyWithoutWriting.error().code(), ErrorCode::InvalidArgument);

    auto negative = segment(SegmentStatus::Writing);
    negative.startTime = TimestampNs{} - std::chrono::nanoseconds{1};
    EXPECT_FALSE(database.beginSegment(sessionId_, negative).hasValue());

    auto tooLarge = segment(SegmentStatus::Writing);
    tooLarge.index = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1u;
    EXPECT_FALSE(database.beginSegment(sessionId_, tooLarge).hasValue());
}

}  // namespace
