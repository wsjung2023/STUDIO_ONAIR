#pragma once

#include "core/Result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace creator::project_store::internal {

enum class SqliteStep { Row, Done };

class SqliteStatement final {
public:
    SqliteStatement(SqliteStatement&& other) noexcept;
    SqliteStatement& operator=(SqliteStatement&& other) noexcept;
    ~SqliteStatement();
    SqliteStatement(const SqliteStatement&) = delete;
    SqliteStatement& operator=(const SqliteStatement&) = delete;

    [[nodiscard]] core::Result<void> bindText(int index, std::string_view value);
    [[nodiscard]] core::Result<void> bindInt64(int index, std::int64_t value);
    [[nodiscard]] core::Result<void> bindDouble(int index, double value);
    [[nodiscard]] core::Result<void> bindNull(int index);
    [[nodiscard]] core::Result<SqliteStep> step();
    [[nodiscard]] std::int64_t columnInt64(int index) const noexcept;
    [[nodiscard]] double columnDouble(int index) const noexcept;
    [[nodiscard]] std::string columnText(int index) const;
    [[nodiscard]] bool columnIsNull(int index) const noexcept;
    [[nodiscard]] core::Result<void> reset();

private:
    friend class SqliteConnection;
    SqliteStatement(sqlite3* database, sqlite3_stmt* statement);
    sqlite3* database_{};
    sqlite3_stmt* statement_{};
};

class SqliteConnection final {
public:
    [[nodiscard]] static core::Result<SqliteConnection> open(
        const std::filesystem::path& databasePath);
    SqliteConnection(SqliteConnection&& other) noexcept;
    SqliteConnection& operator=(SqliteConnection&& other) noexcept;
    ~SqliteConnection();
    SqliteConnection(const SqliteConnection&) = delete;
    SqliteConnection& operator=(const SqliteConnection&) = delete;

    [[nodiscard]] core::Result<void> execute(std::string_view sql);
    [[nodiscard]] core::Result<SqliteStatement> prepare(std::string_view sql);
    [[nodiscard]] core::Result<std::int64_t> scalarInt64(std::string_view sql);
    [[nodiscard]] core::Result<std::string> scalarText(std::string_view sql);
    [[nodiscard]] int changes() const noexcept;

private:
    explicit SqliteConnection(sqlite3* database) : database_(database) {}
    sqlite3* database_{};
};

class SqliteTransaction final {
public:
    [[nodiscard]] static core::Result<SqliteTransaction> beginDeferred(
        SqliteConnection& connection);
    [[nodiscard]] static core::Result<SqliteTransaction> beginImmediate(
        SqliteConnection& connection);
    SqliteTransaction(SqliteTransaction&& other) noexcept;
    ~SqliteTransaction();
    [[nodiscard]] core::Result<void> commit();
    SqliteTransaction(const SqliteTransaction&) = delete;
    SqliteTransaction& operator=(const SqliteTransaction&) = delete;

private:
    explicit SqliteTransaction(SqliteConnection& connection) : connection_(&connection) {}
    SqliteConnection* connection_{};
    bool committed_{false};
};

}  // namespace creator::project_store::internal
