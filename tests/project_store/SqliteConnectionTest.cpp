#include "project_store/internal/SqliteConnection.h"

#include "core/AppError.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace {

namespace fs = std::filesystem;

using creator::core::ErrorCode;
using creator::project_store::internal::SqliteConnection;
using creator::project_store::internal::SqliteStep;
using creator::project_store::internal::SqliteTransaction;

class SqliteConnectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ = fs::temp_directory_path() /
                     ("cs_sqlite_" + std::string{info->test_suite_name()} + "_" +
                      std::string{info->name()});
        std::error_code ec;
        fs::remove_all(directory_, ec);
        fs::create_directories(directory_);
        databasePath_ = directory_ / "project.db";
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(directory_, ec);
    }

    fs::path directory_;
    fs::path databasePath_;
};

TEST_F(SqliteConnectionTest, OpensWithRequiredPragmas) {
    auto opened = SqliteConnection::open(databasePath_);
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto connection = std::move(opened).value();

    EXPECT_EQ(connection.scalarText("PRAGMA journal_mode").value(), "wal");
    EXPECT_EQ(connection.scalarInt64("PRAGMA synchronous").value(), 2);
    EXPECT_EQ(connection.scalarInt64("PRAGMA foreign_keys").value(), 1);
    EXPECT_EQ(connection.scalarInt64("PRAGMA busy_timeout").value(), 2000);
    EXPECT_EQ(connection.scalarText("PRAGMA quick_check").value(), "ok");
}

TEST_F(SqliteConnectionTest, ReportsCorruptDatabaseWithoutThrowing) {
    std::ofstream out{databasePath_, std::ios::binary};
    out << "not sqlite";
    out.close();

    const auto opened = SqliteConnection::open(databasePath_);

    ASSERT_FALSE(opened.hasValue());
    EXPECT_EQ(opened.error().code(), ErrorCode::IoFailure);
}

TEST_F(SqliteConnectionTest, OpensDatabaseBelowNonAsciiPath) {
    const fs::path unicodeDirectory =
        directory_ / fs::path{u8"\uD504\uB85C\uC81D\uD2B8_\U0001F3A5"};
    fs::create_directories(unicodeDirectory);

    auto opened = SqliteConnection::open(unicodeDirectory / "project.db");

    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto connection = std::move(opened).value();
    EXPECT_EQ(connection.scalarText("PRAGMA quick_check").value(), "ok");
}

TEST_F(SqliteConnectionTest, PreparedStatementBindsReadsAndResets) {
    auto opened = SqliteConnection::open(databasePath_);
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto connection = std::move(opened).value();
    ASSERT_TRUE(connection.execute(
        "CREATE TABLE sample(name TEXT NOT NULL, count INTEGER NOT NULL, note TEXT)")
                    .hasValue());

    auto prepared = connection.prepare("INSERT INTO sample VALUES(?1, ?2, ?3)");
    ASSERT_TRUE(prepared.hasValue()) << prepared.error().message();
    auto statement = std::move(prepared).value();
    ASSERT_TRUE(statement.bindText(1, "first").hasValue());
    ASSERT_TRUE(statement.bindInt64(2, 7).hasValue());
    ASSERT_TRUE(statement.bindNull(3).hasValue());
    ASSERT_EQ(statement.step().value(), SqliteStep::Done);
    ASSERT_TRUE(statement.reset().hasValue());
    ASSERT_TRUE(statement.bindText(1, "second").hasValue());
    ASSERT_TRUE(statement.bindInt64(2, 11).hasValue());
    ASSERT_TRUE(statement.bindText(3, "ready").hasValue());
    ASSERT_EQ(statement.step().value(), SqliteStep::Done);

    auto selected = connection.prepare("SELECT name, count, note FROM sample ORDER BY count");
    ASSERT_TRUE(selected.hasValue());
    auto rows = std::move(selected).value();
    ASSERT_EQ(rows.step().value(), SqliteStep::Row);
    EXPECT_EQ(rows.columnText(0), "first");
    EXPECT_EQ(rows.columnInt64(1), 7);
    EXPECT_TRUE(rows.columnIsNull(2));
    ASSERT_EQ(rows.step().value(), SqliteStep::Row);
    EXPECT_EQ(rows.columnText(0), "second");
    EXPECT_EQ(rows.columnInt64(1), 11);
    EXPECT_EQ(rows.columnText(2), "ready");
    EXPECT_EQ(rows.step().value(), SqliteStep::Done);
}

TEST_F(SqliteConnectionTest, TransactionRollsBackUnlessCommitted) {
    auto opened = SqliteConnection::open(databasePath_);
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto connection = std::move(opened).value();
    ASSERT_TRUE(connection.execute("CREATE TABLE sample(value INTEGER NOT NULL)").hasValue());

    {
        auto begun = SqliteTransaction::beginImmediate(connection);
        ASSERT_TRUE(begun.hasValue()) << begun.error().message();
        auto transaction = std::move(begun).value();
        ASSERT_TRUE(connection.execute("INSERT INTO sample VALUES(1)").hasValue());
    }
    ASSERT_EQ(connection.scalarInt64("SELECT COUNT(*) FROM sample").value(), 0);

    auto begun = SqliteTransaction::beginImmediate(connection);
    ASSERT_TRUE(begun.hasValue()) << begun.error().message();
    auto transaction = std::move(begun).value();
    ASSERT_TRUE(connection.execute("INSERT INTO sample VALUES(2)").hasValue());
    ASSERT_TRUE(transaction.commit().hasValue());
    EXPECT_EQ(connection.scalarInt64("SELECT COUNT(*) FROM sample").value(), 1);
}

TEST_F(SqliteConnectionTest, ExecuteRunsEveryStatementInSqlBatch) {
    auto opened = SqliteConnection::open(databasePath_);
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto connection = std::move(opened).value();

    ASSERT_TRUE(connection.execute(
        "CREATE TABLE first_table(value INTEGER);"
        "CREATE TABLE second_table(value INTEGER);"
        "INSERT INTO second_table VALUES(42);")
                    .hasValue());

    const auto value = connection.scalarInt64("SELECT value FROM second_table");
    ASSERT_TRUE(value.hasValue()) << value.error().message();
    EXPECT_EQ(value.value(), 42);
}

}  // namespace
