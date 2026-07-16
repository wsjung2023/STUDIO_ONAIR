#include "project_store/MigrationRunner.h"

#include "core/AppError.h"
#include "project_store/internal/SqliteConnection.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <string>
#include <utility>

namespace {

namespace fs = std::filesystem;

using creator::core::ErrorCode;
using creator::project_store::MigrationRunner;
using creator::project_store::internal::MigrationDescriptor;
using creator::project_store::internal::SqliteConnection;
using creator::project_store::internal::applyMigrations;

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

TEST_F(MigrationRunnerTest, AppliesMigrationOneExactlyOnce) {
    auto opened = SqliteConnection::open(databasePath());
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto connection = std::move(opened).value();

    ASSERT_TRUE(MigrationRunner::apply(connection).hasValue());
    ASSERT_TRUE(MigrationRunner::apply(connection).hasValue());

    EXPECT_EQ(connection.scalarInt64("PRAGMA user_version").value(), 1);
    EXPECT_EQ(connection.scalarInt64(
                  "SELECT count(*) FROM schema_migrations WHERE version=1")
                  .value(),
              1);
    EXPECT_EQ(connection.scalarInt64(
                  "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN "
                  "('projects','recording_sessions','segments')")
                  .value(),
              3);
}

TEST_F(MigrationRunnerTest, RejectsFutureDatabaseWithoutChangingVersion) {
    auto opened = SqliteConnection::open(databasePath());
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto connection = std::move(opened).value();
    ASSERT_TRUE(connection.execute("PRAGMA user_version=2").hasValue());

    const auto result = MigrationRunner::apply(connection);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::UnsupportedVersion);
    EXPECT_EQ(connection.scalarInt64("PRAGMA user_version").value(), 2);
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
    EXPECT_EQ(connection.scalarInt64("PRAGMA user_version").value(), 1);
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
    EXPECT_EQ(connection.scalarInt64("PRAGMA user_version").value(), 1);
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
    EXPECT_EQ(connection.scalarInt64("PRAGMA user_version").value(), 1);
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

}  // namespace
