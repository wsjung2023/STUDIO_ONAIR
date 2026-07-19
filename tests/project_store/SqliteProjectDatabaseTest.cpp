#include "project_store/SqliteProjectDatabase.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "core/Utc.h"
#include "domain/Identifiers.h"
#include "domain/ProjectManifest.h"
#include "project_store/MigrationRunner.h"
#include "project_store/internal/SqliteConnection.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>

namespace {

namespace fs = std::filesystem;

using creator::core::ErrorCode;
using creator::core::TimestampNs;
using creator::core::Utc;
using creator::domain::ProjectId;
using creator::domain::ProjectManifest;
using creator::domain::SessionId;
using creator::project_store::MigrationRunner;
using creator::project_store::PersistedSessionState;
using creator::project_store::SqliteProjectDatabase;
using creator::project_store::internal::SqliteConnection;

Utc utc(std::string_view text) {
    return Utc::parseRfc3339(text).value();
}

ProjectManifest manifest() {
    return ProjectManifest{
        .schemaVersion = ProjectManifest::kCurrentSchemaVersion,
        .projectId = ProjectId::create("123e4567-e89b-42d3-a456-426614174000").value(),
        .name = "Creator Tutorial",
        .createdAt = utc("2026-07-16T09:30:00Z"),
        .updatedAt = utc("2026-07-16T09:30:00Z"),
        .canvas = {},
        .database = std::string{ProjectManifest::kDatabaseFileName},
        .directories = {},
        .requiredFeatures = {},
    };
}

class SqliteProjectDatabaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ = fs::temp_directory_path() /
                     ("cs_project_db_" + std::string{info->test_suite_name()} + "_" +
                      std::string{info->name()});
        std::error_code ec;
        fs::remove_all(directory_, ec);
        fs::create_directories(directory_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(directory_, ec);
    }

    fs::path databasePath() const { return directory_ / "project.db"; }

    fs::path directory_;
};

TEST_F(SqliteProjectDatabaseTest, CreatesProjectRowMatchingManifest) {
    const auto expected = manifest();
    const auto database = SqliteProjectDatabase::create(databasePath(), expected);
    ASSERT_TRUE(database.hasValue()) << database.error().message();

    auto rawResult = SqliteConnection::open(databasePath());
    ASSERT_TRUE(rawResult.hasValue());
    auto raw = std::move(rawResult).value();
    EXPECT_EQ(raw.scalarText("SELECT project_id FROM projects").value(),
              expected.projectId.value());
    EXPECT_EQ(raw.scalarText("SELECT name FROM projects").value(), expected.name);
    EXPECT_EQ(raw.scalarInt64("SELECT manifest_schema_version FROM projects").value(),
              expected.schemaVersion);
}

TEST_F(SqliteProjectDatabaseTest, RejectsManifestDatabaseIdentityMismatch) {
    const auto expected = manifest();
    ASSERT_TRUE(SqliteProjectDatabase::create(databasePath(), expected).hasValue());
    const auto different =
        ProjectId::create("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa").value();

    const auto opened = SqliteProjectDatabase::open(databasePath(), different);

    ASSERT_FALSE(opened.hasValue());
    EXPECT_EQ(opened.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(SqliteProjectDatabaseTest, OpenRejectsMissingOrAmbiguousProjectIdentity) {
    auto rawResult = SqliteConnection::open(databasePath());
    ASSERT_TRUE(rawResult.hasValue());
    auto raw = std::move(rawResult).value();
    ASSERT_TRUE(MigrationRunner::apply(raw).hasValue());

    const auto missing =
        SqliteProjectDatabase::open(databasePath(), manifest().projectId);
    ASSERT_FALSE(missing.hasValue());
    EXPECT_EQ(missing.error().code(), ErrorCode::IoFailure);

    ASSERT_TRUE(raw.execute(
        "INSERT INTO projects VALUES"
        "('123e4567-e89b-42d3-a456-426614174000','one',1,"
        "'2026-07-16T09:30:00Z','2026-07-16T09:30:00Z'),"
        "('aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa','two',1,"
        "'2026-07-16T09:30:00Z','2026-07-16T09:30:00Z')")
                    .hasValue());
    const auto ambiguous =
        SqliteProjectDatabase::open(databasePath(), manifest().projectId);
    ASSERT_FALSE(ambiguous.hasValue());
    EXPECT_EQ(ambiguous.error().code(), ErrorCode::IoFailure);
}

TEST_F(SqliteProjectDatabaseTest, PersistsRecordingCompletedAndAbortedStates) {
    auto created = SqliteProjectDatabase::create(databasePath(), manifest());
    ASSERT_TRUE(created.hasValue()) << created.error().message();
    auto database = std::move(created).value();
    const auto session = SessionId::create("session-1").value();
    const auto started = TimestampNs{} + std::chrono::seconds{1};
    ASSERT_TRUE(database.beginRecording(session, started, utc("2026-07-16T10:00:00Z"))
                    .hasValue());
    auto recording = database.session(session);
    ASSERT_TRUE(recording.hasValue());
    EXPECT_EQ(recording.value().state, PersistedSessionState::Recording);
    EXPECT_EQ(recording.value().startedAt, started);
    EXPECT_FALSE(recording.value().stoppedAt.has_value());

    const auto stopped = TimestampNs{} + std::chrono::seconds{3};
    ASSERT_TRUE(database.completeRecording(session, stopped, {},
                                           utc("2026-07-16T10:00:03Z"))
                    .hasValue());
    auto completed = database.session(session);
    ASSERT_TRUE(completed.hasValue());
    EXPECT_EQ(completed.value().state, PersistedSessionState::Completed);
    EXPECT_EQ(completed.value().stoppedAt, stopped);
    EXPECT_EQ(completed.value().finishedAt, utc("2026-07-16T10:00:03Z"));

    const auto aborted = SessionId::create("session-2").value();
    const auto abortedStart = TimestampNs{} + std::chrono::seconds{60};
    ASSERT_TRUE(database.beginRecording(aborted, abortedStart,
                                        utc("2026-07-16T10:01:00Z"))
                    .hasValue());
    ASSERT_TRUE(database.abortRecording(aborted, "source permission denied",
                                        utc("2026-07-16T10:01:01Z"))
                    .hasValue());
    auto abortedRecord = database.session(aborted);
    ASSERT_TRUE(abortedRecord.hasValue());
    EXPECT_EQ(abortedRecord.value().state, PersistedSessionState::Aborted);
    EXPECT_EQ(abortedRecord.value().stoppedAt, abortedStart);
    ASSERT_TRUE(abortedRecord.value().failureReason.has_value());
    EXPECT_EQ(*abortedRecord.value().failureReason, "source permission denied");
}

TEST_F(SqliteProjectDatabaseTest, GuardsSessionTransitions) {
    auto created = SqliteProjectDatabase::create(databasePath(), manifest());
    ASSERT_TRUE(created.hasValue()) << created.error().message();
    auto database = std::move(created).value();
    const auto session = SessionId::create("session-1").value();
    const auto started = TimestampNs{} + std::chrono::seconds{5};
    ASSERT_TRUE(database.beginRecording(session, started, utc("2026-07-16T10:00:00Z"))
                    .hasValue());

    const auto duplicate =
        database.beginRecording(session, started, utc("2026-07-16T10:00:00Z"));
    ASSERT_FALSE(duplicate.hasValue());
    EXPECT_EQ(duplicate.error().code(), ErrorCode::AlreadyExists);

    const auto backwards = database.completeRecording(
        session, TimestampNs{} + std::chrono::seconds{4}, {},
        utc("2026-07-16T10:00:01Z"));
    ASSERT_FALSE(backwards.hasValue());
    EXPECT_EQ(backwards.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(database.session(session).value().state, PersistedSessionState::Recording);

    ASSERT_TRUE(database.abortRecording(session, "cancelled",
                                        utc("2026-07-16T10:00:02Z"))
                    .hasValue());
    const auto abortAgain = database.abortRecording(
        session, "again", utc("2026-07-16T10:00:03Z"));
    ASSERT_FALSE(abortAgain.hasValue());
    EXPECT_EQ(abortAgain.error().code(), ErrorCode::InvalidState);
}

TEST_F(SqliteProjectDatabaseTest, SessionLookupReportsMissingId) {
    auto created = SqliteProjectDatabase::create(databasePath(), manifest());
    ASSERT_TRUE(created.hasValue());
    auto database = std::move(created).value();

    const auto result = database.session(SessionId::create("missing").value());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
}

}  // namespace
