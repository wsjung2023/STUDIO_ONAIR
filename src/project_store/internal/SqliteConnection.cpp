#include "project_store/internal/SqliteConnection.h"

#include "core/AppError.h"

#include <sqlite3.h>

#include <climits>
#include <string>
#include <utility>

namespace creator::project_store::internal {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

AppError sqliteError(sqlite3* database, std::string_view operation, int code) {
    std::string message{"sqlite "};
    message += operation;
    message += " failed (code ";
    message += std::to_string(code);
    message += ")";
    if (database != nullptr) {
        message += ": ";
        message += sqlite3_errmsg(database);
    }
    return AppError{ErrorCode::IoFailure, std::move(message)};
}

Result<void> checkSqlite(sqlite3* database, std::string_view operation, int code) {
    if (code == SQLITE_OK) {
        return core::ok();
    }
    return sqliteError(database, operation, code);
}

}  // namespace

SqliteStatement::SqliteStatement(sqlite3* database, sqlite3_stmt* statement)
    : database_(database), statement_(statement) {}

SqliteStatement::SqliteStatement(SqliteStatement&& other) noexcept
    : database_(std::exchange(other.database_, nullptr)),
      statement_(std::exchange(other.statement_, nullptr)) {}

SqliteStatement& SqliteStatement::operator=(SqliteStatement&& other) noexcept {
    if (this != &other) {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
        database_ = std::exchange(other.database_, nullptr);
        statement_ = std::exchange(other.statement_, nullptr);
    }
    return *this;
}

SqliteStatement::~SqliteStatement() {
    if (statement_ != nullptr) {
        sqlite3_finalize(statement_);
    }
}

Result<void> SqliteStatement::bindText(int index, std::string_view value) {
    if (value.size() > static_cast<std::size_t>(INT_MAX)) {
        return sqliteError(database_, "bind text", SQLITE_TOOBIG);
    }
    return checkSqlite(database_, "bind text",
                       sqlite3_bind_text(statement_, index, value.data(),
                                         static_cast<int>(value.size()), SQLITE_TRANSIENT));
}

Result<void> SqliteStatement::bindInt64(int index, std::int64_t value) {
    return checkSqlite(database_, "bind integer", sqlite3_bind_int64(statement_, index, value));
}

Result<void> SqliteStatement::bindNull(int index) {
    return checkSqlite(database_, "bind null", sqlite3_bind_null(statement_, index));
}

Result<SqliteStep> SqliteStatement::step() {
    const int code = sqlite3_step(statement_);
    if (code == SQLITE_ROW) {
        return SqliteStep::Row;
    }
    if (code == SQLITE_DONE) {
        return SqliteStep::Done;
    }
    return sqliteError(database_, "step statement", code);
}

std::int64_t SqliteStatement::columnInt64(int index) const noexcept {
    return sqlite3_column_int64(statement_, index);
}

std::string SqliteStatement::columnText(int index) const {
    const auto* text = sqlite3_column_text(statement_, index);
    if (text == nullptr) {
        return {};
    }
    const int bytes = sqlite3_column_bytes(statement_, index);
    return std::string{reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes)};
}

bool SqliteStatement::columnIsNull(int index) const noexcept {
    return sqlite3_column_type(statement_, index) == SQLITE_NULL;
}

Result<void> SqliteStatement::reset() {
    const int resetCode = sqlite3_reset(statement_);
    if (resetCode != SQLITE_OK) {
        return sqliteError(database_, "reset statement", resetCode);
    }
    return checkSqlite(database_, "clear statement bindings", sqlite3_clear_bindings(statement_));
}

Result<SqliteConnection> SqliteConnection::open(const std::filesystem::path& databasePath) {
    sqlite3* database = nullptr;
#ifdef _WIN32
    const int openCode = sqlite3_open16(databasePath.c_str(), &database);
#else
    const auto utf8 = databasePath.u8string();
    const int openCode =
        sqlite3_open(reinterpret_cast<const char*>(utf8.c_str()), &database);
#endif
    if (openCode != SQLITE_OK) {
        AppError error = sqliteError(database, "open database", openCode);
        if (database != nullptr) {
            sqlite3_close_v2(database);
        }
        return error;
    }

    SqliteConnection connection{database};
    if (const int timeoutCode = sqlite3_busy_timeout(database, 2000); timeoutCode != SQLITE_OK) {
        return sqliteError(database, "set busy timeout", timeoutCode);
    }
    if (auto foreignKeys = connection.execute("PRAGMA foreign_keys = ON");
        !foreignKeys.hasValue()) {
        return foreignKeys.error();
    }
    auto journalMode = connection.scalarText("PRAGMA journal_mode = WAL");
    if (!journalMode.hasValue()) {
        return journalMode.error();
    }
    if (journalMode.value() != "wal") {
        return AppError{ErrorCode::IoFailure, "sqlite journal mode is not WAL"};
    }
    if (auto synchronous = connection.execute("PRAGMA synchronous = FULL");
        !synchronous.hasValue()) {
        return synchronous.error();
    }
    auto quickCheck = connection.scalarText("PRAGMA quick_check");
    if (!quickCheck.hasValue()) {
        return quickCheck.error();
    }
    if (quickCheck.value() != "ok") {
        return AppError{ErrorCode::IoFailure, "sqlite quick_check reported corruption"};
    }
    return connection;
}

SqliteConnection::SqliteConnection(SqliteConnection&& other) noexcept
    : database_(std::exchange(other.database_, nullptr)) {}

SqliteConnection& SqliteConnection::operator=(SqliteConnection&& other) noexcept {
    if (this != &other) {
        if (database_ != nullptr) {
            sqlite3_close_v2(database_);
        }
        database_ = std::exchange(other.database_, nullptr);
    }
    return *this;
}

SqliteConnection::~SqliteConnection() {
    if (database_ != nullptr) {
        sqlite3_close_v2(database_);
    }
}

Result<void> SqliteConnection::execute(std::string_view sql) {
    // sqlite3_exec deliberately handles the whole batch. Preparing a single
    // statement and ignoring sqlite3_prepare_v2's tail would make a migration
    // containing several CREATE TABLE statements report success after only
    // the first one ran.
    const std::string batch{sql};
    char* detail = nullptr;
    const int code = sqlite3_exec(database_, batch.c_str(), nullptr, nullptr, &detail);
    sqlite3_free(detail);
    return checkSqlite(database_, "execute SQL batch", code);
}

Result<SqliteStatement> SqliteConnection::prepare(std::string_view sql) {
    if (sql.size() > static_cast<std::size_t>(INT_MAX)) {
        return sqliteError(database_, "prepare statement", SQLITE_TOOBIG);
    }
    sqlite3_stmt* statement = nullptr;
    const int code = sqlite3_prepare_v2(database_, sql.data(), static_cast<int>(sql.size()),
                                        &statement, nullptr);
    if (code != SQLITE_OK) {
        if (statement != nullptr) {
            sqlite3_finalize(statement);
        }
        return sqliteError(database_, "prepare statement", code);
    }
    return SqliteStatement{database_, statement};
}

Result<std::int64_t> SqliteConnection::scalarInt64(std::string_view sql) {
    auto prepared = prepare(sql);
    if (!prepared.hasValue()) {
        return prepared.error();
    }
    auto statement = std::move(prepared).value();
    auto stepped = statement.step();
    if (!stepped.hasValue()) {
        return stepped.error();
    }
    if (stepped.value() != SqliteStep::Row) {
        return AppError{ErrorCode::IoFailure, "sqlite integer query returned no row"};
    }
    return statement.columnInt64(0);
}

Result<std::string> SqliteConnection::scalarText(std::string_view sql) {
    auto prepared = prepare(sql);
    if (!prepared.hasValue()) {
        return prepared.error();
    }
    auto statement = std::move(prepared).value();
    auto stepped = statement.step();
    if (!stepped.hasValue()) {
        return stepped.error();
    }
    if (stepped.value() != SqliteStep::Row) {
        return AppError{ErrorCode::IoFailure, "sqlite text query returned no row"};
    }
    return statement.columnText(0);
}

int SqliteConnection::changes() const noexcept {
    return sqlite3_changes(database_);
}

Result<SqliteTransaction> SqliteTransaction::beginImmediate(SqliteConnection& connection) {
    if (auto begun = connection.execute("BEGIN IMMEDIATE"); !begun.hasValue()) {
        return begun.error();
    }
    return SqliteTransaction{connection};
}

SqliteTransaction::SqliteTransaction(SqliteTransaction&& other) noexcept
    : connection_(std::exchange(other.connection_, nullptr)), committed_(other.committed_) {
    other.committed_ = true;
}

SqliteTransaction::~SqliteTransaction() {
    if (connection_ != nullptr && !committed_) {
        const auto rolledBack = connection_->execute("ROLLBACK");
        (void)rolledBack;
    }
}

Result<void> SqliteTransaction::commit() {
    if (connection_ == nullptr || committed_) {
        return AppError{ErrorCode::InvalidState, "sqlite transaction is not active"};
    }
    auto committed = connection_->execute("COMMIT");
    if (!committed.hasValue()) {
        return committed.error();
    }
    committed_ = true;
    return core::ok();
}

}  // namespace creator::project_store::internal
