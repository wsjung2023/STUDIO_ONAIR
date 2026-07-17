#include "project_store/MigrationRunner.h"

#include "core/AppError.h"
#include "core/Utc.h"
#include "project_store/Migration001.h"
#include "project_store/Migration002.h"
#include "project_store/Migration003.h"
#include "project_store/internal/SqliteConnection.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace creator::project_store {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;
using internal::MigrationDescriptor;
using internal::SqliteConnection;
using internal::SqliteStep;
using internal::SqliteTransaction;

Result<void> invalidMetadata(std::int32_t version) {
    return AppError{ErrorCode::IoFailure,
                    "sqlite migration metadata is invalid for version " +
                        std::to_string(version)};
}

Result<void> verifyAppliedMigration(SqliteConnection& connection,
                                    const MigrationDescriptor& migration) {
    auto prepared = connection.prepare(
        "SELECT name, checksum FROM schema_migrations WHERE version=?1");
    if (!prepared.hasValue()) {
        return prepared.error();
    }
    auto statement = std::move(prepared).value();
    if (auto bound = statement.bindInt64(1, migration.version); !bound.hasValue()) {
        return bound.error();
    }
    auto row = statement.step();
    if (!row.hasValue()) {
        return row.error();
    }
    if (row.value() != SqliteStep::Row || statement.columnText(0) != migration.name ||
        statement.columnText(1) != migration.checksum) {
        return invalidMetadata(migration.version);
    }
    auto end = statement.step();
    if (!end.hasValue()) {
        return end.error();
    }
    if (end.value() != SqliteStep::Done) {
        return invalidMetadata(migration.version);
    }
    return core::ok();
}

Result<void> recordAppliedMigration(SqliteConnection& connection,
                                    const MigrationDescriptor& migration) {
    auto prepared = connection.prepare(
        "INSERT INTO schema_migrations(version, name, checksum, applied_at_utc) "
        "VALUES(?1, ?2, ?3, ?4)");
    if (!prepared.hasValue()) {
        return prepared.error();
    }
    auto statement = std::move(prepared).value();
    if (auto bound = statement.bindInt64(1, migration.version); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(2, migration.name); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(3, migration.checksum); !bound.hasValue()) {
        return bound.error();
    }
    const std::string appliedAt = core::Utc::now().toRfc3339();
    if (auto bound = statement.bindText(4, appliedAt); !bound.hasValue()) {
        return bound.error();
    }
    auto stepped = statement.step();
    if (!stepped.hasValue()) {
        return stepped.error();
    }
    if (stepped.value() != SqliteStep::Done) {
        return AppError{ErrorCode::IoFailure,
                        "sqlite migration metadata insert unexpectedly returned a row"};
    }
    return core::ok();
}

Result<void> validateDescriptors(std::span<const MigrationDescriptor> migrations) {
    for (std::size_t index = 0; index < migrations.size(); ++index) {
        const auto& migration = migrations[index];
        const auto expectedVersion = static_cast<std::int32_t>(index + 1);
        if (migration.version != expectedVersion || migration.name.empty() ||
            migration.checksum.empty() || migration.sql.empty()) {
            return AppError{ErrorCode::InvalidArgument,
                            "migration descriptors must be complete and contiguous from version 1"};
        }
    }
    return core::ok();
}

}  // namespace

namespace internal {

std::span<const MigrationDescriptor> defaultMigrations() noexcept {
    static constexpr std::array migrations{
        MigrationDescriptor{.version = embedded::kMigration001Version,
                            .name = embedded::kMigration001Name,
                            .checksum = embedded::kMigration001Sha256,
                            .sql = embedded::kMigration001Sql},
        MigrationDescriptor{.version = embedded::kMigration002Version,
                            .name = embedded::kMigration002Name,
                            .checksum = embedded::kMigration002Sha256,
                            .sql = embedded::kMigration002Sql},
        MigrationDescriptor{.version = embedded::kMigration003Version,
                            .name = embedded::kMigration003Name,
                            .checksum = embedded::kMigration003Sha256,
                            .sql = embedded::kMigration003Sql}};
    static_assert(migrations.back().version == MigrationRunner::kLatestVersion);
    return migrations;
}

Result<void> applyMigrations(SqliteConnection& connection,
                             std::span<const MigrationDescriptor> migrations) {
    if (auto valid = validateDescriptors(migrations); !valid.hasValue()) {
        return valid.error();
    }

    auto currentResult = connection.scalarInt64("PRAGMA user_version");
    if (!currentResult.hasValue()) {
        return currentResult.error();
    }
    const std::int64_t currentVersion = currentResult.value();
    const std::int64_t latestVersion =
        migrations.empty() ? 0 : static_cast<std::int64_t>(migrations.back().version);
    if (currentVersion < 0) {
        return AppError{ErrorCode::IoFailure, "sqlite user_version cannot be negative"};
    }
    if (currentVersion > latestVersion) {
        return AppError{ErrorCode::UnsupportedVersion,
                        "project database was written by a newer Creator Studio version"};
    }

    if (currentVersion > 0) {
        auto metadataCount =
            connection.scalarInt64("SELECT count(*) FROM schema_migrations");
        if (!metadataCount.hasValue()) {
            return metadataCount.error();
        }
        if (metadataCount.value() != currentVersion) {
            return invalidMetadata(static_cast<std::int32_t>(currentVersion));
        }
    }

    for (const auto& migration : migrations) {
        if (migration.version > currentVersion) {
            break;
        }
        if (auto verified = verifyAppliedMigration(connection, migration); !verified.hasValue()) {
            return verified.error();
        }
    }

    for (const auto& migration : migrations) {
        if (migration.version <= currentVersion) {
            continue;
        }
        auto begun = SqliteTransaction::beginImmediate(connection);
        if (!begun.hasValue()) {
            return begun.error();
        }
        auto transaction = std::move(begun).value();
        if (auto executed = connection.execute(migration.sql); !executed.hasValue()) {
            return executed.error();
        }
        if (auto recorded = recordAppliedMigration(connection, migration); !recorded.hasValue()) {
            return recorded.error();
        }
        if (auto versioned = connection.execute("PRAGMA user_version=" +
                                                std::to_string(migration.version));
            !versioned.hasValue()) {
            return versioned.error();
        }
        if (auto committed = transaction.commit(); !committed.hasValue()) {
            return committed.error();
        }
    }
    return core::ok();
}

}  // namespace internal

Result<void> MigrationRunner::apply(internal::SqliteConnection& connection) {
    return internal::applyMigrations(connection, internal::defaultMigrations());
}

}  // namespace creator::project_store
