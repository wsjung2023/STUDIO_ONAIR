#include "project_store/MigrationRunner.h"

#include "core/AppError.h"
#include "project_store/internal/SqliteConnection.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <utility>

namespace {

namespace fs = std::filesystem;

using creator::core::ErrorCode;
using creator::project_store::MigrationRunner;
using creator::project_store::internal::MigrationDescriptor;
using creator::project_store::internal::SqliteConnection;
using creator::project_store::internal::applyMigrations;
using creator::project_store::internal::defaultMigrations;

class MigrationRunnerTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ = fs::temp_directory_path() /
                     ("cs_migration_" + std::string{info->test_suite_name()} + "_" +
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

TEST_F(MigrationRunnerTest, AppliesAllMigrationsExactlyOnce) {
    auto opened = SqliteConnection::open(databasePath());
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto connection = std::move(opened).value();

    ASSERT_TRUE(MigrationRunner::apply(connection).hasValue());
    ASSERT_TRUE(MigrationRunner::apply(connection).hasValue());

    EXPECT_EQ(connection.scalarInt64("PRAGMA user_version").value(), 2);
    EXPECT_EQ(connection.scalarInt64(
                  "SELECT count(*) FROM schema_migrations WHERE version=1")
                  .value(),
              1);
    EXPECT_EQ(connection.scalarInt64(
                  "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN "
                  "('projects','recording_sessions','segments')")
                  .value(),
              3);
    EXPECT_EQ(connection.scalarInt64(
                  "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN "
                  "('media_assets','timelines','tracks','clips',"
                  "'clip_visual_transforms','clip_audio_envelopes','titles',"
                  "'caption_cues','markers','edit_commands','edit_checkpoints')")
                  .value(),
              11);
    EXPECT_EQ(connection.scalarInt64(
                  "SELECT count(*) FROM schema_migrations WHERE version=2")
                  .value(),
              1);
}

TEST_F(MigrationRunnerTest, RejectsFutureDatabaseWithoutChangingVersion) {
    auto opened = SqliteConnection::open(databasePath());
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto connection = std::move(opened).value();
    ASSERT_TRUE(connection.execute("PRAGMA user_version=3").hasValue());

    const auto result = MigrationRunner::apply(connection);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::UnsupportedVersion);
    EXPECT_EQ(connection.scalarInt64("PRAGMA user_version").value(), 3);
    EXPECT_EQ(connection.scalarInt64(
                  "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='projects'")
                  .value(),
              0);
}

TEST_F(MigrationRunnerTest, RejectsChangedChecksum) {
    auto opened = SqliteConnection::open(databasePath());
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto connection = std::move(opened).value();
    ASSERT_TRUE(MigrationRunner::apply(connection).hasValue());
    ASSERT_TRUE(connection.execute(
        "UPDATE schema_migrations SET checksum='wrong' WHERE version=1")
                    .hasValue());

    const auto result = MigrationRunner::apply(connection);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::IoFailure);
    EXPECT_EQ(connection.scalarInt64("PRAGMA user_version").value(), 2);
}

TEST_F(MigrationRunnerTest, RejectsChangedSecondMigrationChecksum) {
    auto opened = SqliteConnection::open(databasePath());
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto connection = std::move(opened).value();
    ASSERT_TRUE(MigrationRunner::apply(connection).hasValue());
    ASSERT_TRUE(connection.execute(
        "UPDATE schema_migrations SET checksum='wrong' WHERE version=2")
                    .hasValue());

    const auto result = MigrationRunner::apply(connection);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::IoFailure);
    EXPECT_EQ(connection.scalarInt64("PRAGMA user_version").value(), 2);
}

TEST_F(MigrationRunnerTest, UpgradesVersionOneWithoutLosingExistingRows) {
    auto opened = SqliteConnection::open(databasePath());
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto connection = std::move(opened).value();
    const auto migrations = defaultMigrations();
    ASSERT_EQ(migrations.size(), 2U);
    ASSERT_TRUE(applyMigrations(connection, migrations.first(1)).hasValue());
    ASSERT_TRUE(connection.execute(
        "INSERT INTO projects VALUES("
        "'project-1','Existing',1,'2026-07-16T09:30:00Z','2026-07-16T09:30:00Z');"
        "INSERT INTO recording_sessions VALUES("
        "'session-1','project-1','COMPLETED',0,10,"
        "'2026-07-16T09:30:00Z','2026-07-16T09:30:01Z',NULL);"
        "INSERT INTO segments VALUES("
        "'session-1','screen',0,0,10,'READY','media/screen-0000.mkv');")
                    .hasValue());

    ASSERT_TRUE(MigrationRunner::apply(connection).hasValue());

    EXPECT_EQ(connection.scalarInt64("PRAGMA user_version").value(), 2);
    EXPECT_EQ(connection.scalarInt64("SELECT count(*) FROM projects").value(), 1);
    EXPECT_EQ(connection.scalarInt64("SELECT count(*) FROM recording_sessions").value(), 1);
    EXPECT_EQ(connection.scalarInt64("SELECT count(*) FROM segments").value(), 1);
}

TEST_F(MigrationRunnerTest, TimelineSchemaRejectsOrphansAndInvalidRanges) {
    auto opened = SqliteConnection::open(databasePath());
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto connection = std::move(opened).value();
    ASSERT_TRUE(MigrationRunner::apply(connection).hasValue());

    const auto orphan = connection.execute(
        "INSERT INTO tracks(track_id,timeline_id,kind,name,position,enabled,locked) "
        "VALUES('track','missing','VIDEO','V1',0,1,0)");
    ASSERT_FALSE(orphan.hasValue());

    ASSERT_TRUE(connection.execute(
        "INSERT INTO projects VALUES("
        "'project-1','Existing',1,'2026-07-16T09:30:00Z','2026-07-16T09:30:00Z');"
        "INSERT INTO timelines(timeline_id,project_id,name,frame_rate_numerator,"
        "frame_rate_denominator,revision,is_primary) "
        "VALUES('timeline','project-1','Main',60,1,0,1);"
        "INSERT INTO media_assets(asset_id,project_id,kind,relative_path,duration_ns,"
        "width,height,frame_rate_numerator,frame_rate_denominator,sample_rate,channels,"
        "file_size,fingerprint,availability) VALUES("
        "'asset','project-1','VIDEO','media/a.mkv',100,1920,1080,60,1,NULL,NULL,"
        "1000,'hash','AVAILABLE');"
        "INSERT INTO tracks(track_id,timeline_id,kind,name,position,enabled,locked) "
        "VALUES('track','timeline','VIDEO','V1',0,1,0);")
                    .hasValue());
    const auto invalidRange = connection.execute(
        "INSERT INTO clips(clip_id,track_id,clip_kind,asset_id,media_kind,"
        "source_start_ns,source_duration_ns,timeline_start_ns,timeline_duration_ns,enabled) "
        "VALUES('clip','track','ASSET','asset','VIDEO',0,0,0,0,1)");
    ASSERT_FALSE(invalidRange.hasValue());
}

TEST_F(MigrationRunnerTest, AllowsOnlyOnePrimaryTimelinePerProject) {
    auto opened = SqliteConnection::open(databasePath());
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto connection = std::move(opened).value();
    ASSERT_TRUE(MigrationRunner::apply(connection).hasValue());
    ASSERT_TRUE(connection.execute(
        "INSERT INTO projects VALUES("
        "'project-1','Existing',1,'2026-07-16T09:30:00Z','2026-07-16T09:30:00Z');"
        "INSERT INTO timelines(timeline_id,project_id,name,frame_rate_numerator,"
        "frame_rate_denominator,revision,is_primary) "
        "VALUES('timeline-1','project-1','Main',60,1,0,1);")
                    .hasValue());

    const auto duplicate = connection.execute(
        "INSERT INTO timelines(timeline_id,project_id,name,frame_rate_numerator,"
        "frame_rate_denominator,revision,is_primary) "
        "VALUES('timeline-2','project-1','Other',60,1,0,1)");

    ASSERT_FALSE(duplicate.hasValue());
}

TEST_F(MigrationRunnerTest, TimelineSchemaRejectsCrossProjectAndOutOfAssetClips) {
    auto opened = SqliteConnection::open(databasePath());
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto connection = std::move(opened).value();
    ASSERT_TRUE(MigrationRunner::apply(connection).hasValue());
    ASSERT_TRUE(connection.execute(
        "INSERT INTO projects VALUES"
        "('project-1','One',1,'2026-07-16T09:30:00Z','2026-07-16T09:30:00Z'),"
        "('project-2','Two',1,'2026-07-16T09:30:00Z','2026-07-16T09:30:00Z');"
        "INSERT INTO timelines(timeline_id,project_id,name,frame_rate_numerator,"
        "frame_rate_denominator,revision,is_primary) "
        "VALUES('timeline','project-1','Main',60,1,0,1);"
        "INSERT INTO tracks(track_id,timeline_id,kind,name,position,enabled,locked) VALUES"
        "('video-track','timeline','VIDEO','V1',0,1,0),"
        "('audio-track','timeline','AUDIO','A1',1,1,0);"
        "INSERT INTO media_assets(asset_id,project_id,kind,relative_path,duration_ns,"
        "width,height,frame_rate_numerator,frame_rate_denominator,sample_rate,channels,"
        "file_size,fingerprint,availability) VALUES"
        "('local','project-1','VIDEO','media/local.mkv',100,1920,1080,60,1,"
        "NULL,NULL,1000,'local-hash','AVAILABLE'),"
        "('other','project-2','VIDEO','media/other.mkv',100,1920,1080,60,1,"
        "NULL,NULL,1000,'other-hash','AVAILABLE');")
                    .hasValue());

    const auto crossProject = connection.execute(
        "INSERT INTO clips VALUES("
        "'cross','video-track','ASSET','other','VIDEO',0,10,0,10,1)");
    const auto outsideAsset = connection.execute(
        "INSERT INTO clips VALUES("
        "'outside','video-track','ASSET','local','VIDEO',95,10,0,10,1)");
    const auto wrongTrack = connection.execute(
        "INSERT INTO clips VALUES("
        "'wrong-track','audio-track','ASSET','local','VIDEO',0,10,0,10,1)");

    EXPECT_FALSE(crossProject.hasValue());
    EXPECT_FALSE(outsideAsset.hasValue());
    EXPECT_FALSE(wrongTrack.hasValue());
}

TEST_F(MigrationRunnerTest, RejectsMissingMigrationMetadataAtCurrentVersion) {
    auto opened = SqliteConnection::open(databasePath());
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto connection = std::move(opened).value();
    ASSERT_TRUE(MigrationRunner::apply(connection).hasValue());
    ASSERT_TRUE(connection.execute("DELETE FROM schema_migrations WHERE version=1").hasValue());

    const auto result = MigrationRunner::apply(connection);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::IoFailure);
    EXPECT_EQ(connection.scalarInt64("PRAGMA user_version").value(), 2);
}

TEST_F(MigrationRunnerTest, RejectsExtraMigrationMetadataAtCurrentVersion) {
    auto opened = SqliteConnection::open(databasePath());
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto connection = std::move(opened).value();
    ASSERT_TRUE(MigrationRunner::apply(connection).hasValue());
    ASSERT_TRUE(connection.execute(
        "INSERT INTO schema_migrations VALUES(99, 'unexpected', 'unexpected', "
        "'2026-07-16T09:30:00Z')")
                    .hasValue());

    const auto result = MigrationRunner::apply(connection);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::IoFailure);
    EXPECT_EQ(connection.scalarInt64("PRAGMA user_version").value(), 2);
}

TEST_F(MigrationRunnerTest, InvalidMigrationRollsBackSchemaAndVersion) {
    auto opened = SqliteConnection::open(databasePath());
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto connection = std::move(opened).value();
    constexpr std::array migrations{
        MigrationDescriptor{.version = 1,
                            .name = "001_broken",
                            .checksum = "test-checksum",
                            .sql = "CREATE TABLE partial(value INTEGER); NOT VALID SQL;"}};

    const auto result = applyMigrations(connection, migrations);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::IoFailure);
    EXPECT_EQ(connection.scalarInt64("PRAGMA user_version").value(), 0);
    EXPECT_EQ(connection.scalarInt64(
                  "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='partial'")
                  .value(),
              0);
}

TEST_F(MigrationRunnerTest, InvalidSecondMigrationKeepsCommittedVersionOne) {
    auto opened = SqliteConnection::open(databasePath());
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto connection = std::move(opened).value();
    constexpr std::array migrations{
        MigrationDescriptor{
            .version = 1,
            .name = "001_test",
            .checksum = "one",
            .sql = "CREATE TABLE schema_migrations("
                   "version INTEGER PRIMARY KEY,name TEXT NOT NULL,"
                   "checksum TEXT NOT NULL,applied_at_utc TEXT NOT NULL);"
                   "CREATE TABLE base(value INTEGER);"},
        MigrationDescriptor{.version = 2,
                            .name = "002_broken",
                            .checksum = "two",
                            .sql = "CREATE TABLE partial(value INTEGER); NOT VALID SQL;"}};

    const auto result = applyMigrations(connection, migrations);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::IoFailure);
    EXPECT_EQ(connection.scalarInt64("PRAGMA user_version").value(), 1);
    EXPECT_EQ(connection.scalarInt64(
                  "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='base'")
                  .value(),
              1);
    EXPECT_EQ(connection.scalarInt64(
                  "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='partial'")
                  .value(),
              0);
}

}  // namespace
