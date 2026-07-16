#include "project_store/SqliteProjectDatabase.h"

#include "core/AppError.h"
#include "project_store/MigrationRunner.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace creator::project_store {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;
using core::TimestampNs;
using domain::ProjectId;
using domain::ProjectManifest;
using domain::SessionId;
using internal::SqliteConnection;
using internal::SqliteStep;
using internal::SqliteTransaction;

std::int64_t toNanoseconds(TimestampNs value) noexcept {
    return value.time_since_epoch().count();
}

TimestampNs fromNanoseconds(std::int64_t value) noexcept {
    return TimestampNs{core::DurationNs{value}};
}

Result<void> expectDone(internal::SqliteStatement& statement, std::string_view operation) {
    auto stepped = statement.step();
    if (!stepped.hasValue()) {
        return stepped.error();
    }
    if (stepped.value() != SqliteStep::Done) {
        return AppError{ErrorCode::IoFailure,
                        "sqlite " + std::string{operation} + " unexpectedly returned a row"};
    }
    return core::ok();
}

Result<bool> sessionExists(SqliteConnection& connection, const SessionId& sessionId) {
    auto prepared = connection.prepare(
        "SELECT count(*) FROM recording_sessions WHERE session_id=?1");
    if (!prepared.hasValue()) {
        return prepared.error();
    }
    auto statement = std::move(prepared).value();
    if (auto bound = statement.bindText(1, sessionId.value()); !bound.hasValue()) {
        return bound.error();
    }
    auto stepped = statement.step();
    if (!stepped.hasValue()) {
        return stepped.error();
    }
    if (stepped.value() != SqliteStep::Row) {
        return AppError{ErrorCode::IoFailure, "sqlite session existence query returned no row"};
    }
    return statement.columnInt64(0) != 0;
}

Result<PersistedSessionState> parseState(std::string_view state) {
    if (state == "RECORDING") return PersistedSessionState::Recording;
    if (state == "COMPLETED") return PersistedSessionState::Completed;
    if (state == "RECOVERED") return PersistedSessionState::Recovered;
    if (state == "ABORTED") return PersistedSessionState::Aborted;
    return AppError{ErrorCode::IoFailure, "sqlite recording session has an unknown state"};
}

Result<core::Utc> parsePersistedUtc(std::string_view value, std::string_view field) {
    auto parsed = core::Utc::parseRfc3339(value);
    if (!parsed.hasValue()) {
        return AppError{ErrorCode::IoFailure,
                        "sqlite recording session has invalid " + std::string{field}};
    }
    return parsed.value();
}

}  // namespace

SqliteProjectDatabase::SqliteProjectDatabase(SqliteConnection connection, ProjectId projectId)
    : connection_(std::move(connection)), projectId_(std::move(projectId)) {}

Result<SqliteProjectDatabase> SqliteProjectDatabase::create(
    const std::filesystem::path& databasePath, const ProjectManifest& manifest) {
    if (auto valid = domain::validate(manifest); !valid.hasValue()) {
        return valid.error();
    }
    auto opened = SqliteConnection::open(databasePath);
    if (!opened.hasValue()) {
        return opened.error();
    }
    auto connection = std::move(opened).value();
    if (auto migrated = MigrationRunner::apply(connection); !migrated.hasValue()) {
        return migrated.error();
    }
    auto begun = SqliteTransaction::beginImmediate(connection);
    if (!begun.hasValue()) {
        return begun.error();
    }
    auto transaction = std::move(begun).value();
    auto rowCount = connection.scalarInt64("SELECT count(*) FROM projects");
    if (!rowCount.hasValue()) {
        return rowCount.error();
    }
    if (rowCount.value() != 0) {
        return AppError{ErrorCode::AlreadyExists,
                        "project database already contains project identity"};
    }
    auto prepared = connection.prepare(
        "INSERT INTO projects(project_id, name, manifest_schema_version, created_at_utc, "
        "updated_at_utc) VALUES(?1, ?2, ?3, ?4, ?5)");
    if (!prepared.hasValue()) {
        return prepared.error();
    }
    auto statement = std::move(prepared).value();
    if (auto bound = statement.bindText(1, manifest.projectId.value()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(2, manifest.name); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindInt64(3, manifest.schemaVersion); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(4, manifest.createdAt.toRfc3339());
        !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(5, manifest.updatedAt.toRfc3339());
        !bound.hasValue()) {
        return bound.error();
    }
    if (auto inserted = expectDone(statement, "insert project identity");
        !inserted.hasValue()) {
        return inserted.error();
    }
    if (auto committed = transaction.commit(); !committed.hasValue()) {
        return committed.error();
    }
    return SqliteProjectDatabase{std::move(connection), manifest.projectId};
}

Result<SqliteProjectDatabase> SqliteProjectDatabase::open(
    const std::filesystem::path& databasePath, const ProjectId& expectedProjectId) {
    auto opened = SqliteConnection::open(databasePath);
    if (!opened.hasValue()) {
        return opened.error();
    }
    auto connection = std::move(opened).value();
    if (auto migrated = MigrationRunner::apply(connection); !migrated.hasValue()) {
        return migrated.error();
    }
    auto prepared = connection.prepare("SELECT project_id FROM projects");
    if (!prepared.hasValue()) {
        return prepared.error();
    }
    auto statement = std::move(prepared).value();
    auto row = statement.step();
    if (!row.hasValue()) {
        return row.error();
    }
    if (row.value() != SqliteStep::Row) {
        return AppError{ErrorCode::IoFailure,
                        "project database contains no project identity"};
    }
    const std::string storedProjectId = statement.columnText(0);
    auto end = statement.step();
    if (!end.hasValue()) {
        return end.error();
    }
    if (end.value() != SqliteStep::Done) {
        return AppError{ErrorCode::IoFailure,
                        "project database contains multiple project identities"};
    }
    if (storedProjectId != expectedProjectId.value()) {
        return AppError{ErrorCode::InvalidArgument,
                        "manifest project identity does not match project database"};
    }
    return SqliteProjectDatabase{std::move(connection), expectedProjectId};
}

Result<void> SqliteProjectDatabase::beginRecording(const SessionId& sessionId,
                                                   TimestampNs startedAt,
                                                   const core::Utc& createdAt) {
    if (toNanoseconds(startedAt) < 0) {
        return AppError{ErrorCode::InvalidArgument,
                        "recording start time cannot be negative"};
    }
    auto prepared = connection_.prepare(
        "INSERT INTO recording_sessions(session_id, project_id, state, started_ns, "
        "stopped_ns, created_at_utc, finished_at_utc, failure_reason) "
        "VALUES(?1, ?2, 'RECORDING', ?3, NULL, ?4, NULL, NULL)");
    if (!prepared.hasValue()) {
        return prepared.error();
    }
    auto statement = std::move(prepared).value();
    if (auto bound = statement.bindText(1, sessionId.value()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(2, projectId_.value()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindInt64(3, toNanoseconds(startedAt)); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(4, createdAt.toRfc3339()); !bound.hasValue()) {
        return bound.error();
    }
    auto inserted = expectDone(statement, "begin recording");
    if (!inserted.hasValue()) {
        auto exists = sessionExists(connection_, sessionId);
        if (exists.hasValue() && exists.value()) {
            return AppError{ErrorCode::AlreadyExists,
                            "recording session identity already exists"};
        }
        return inserted.error();
    }
    return core::ok();
}

Result<void> SqliteProjectDatabase::completeRecording(
    const SessionId& sessionId, TimestampNs stoppedAt,
    const std::vector<domain::SegmentInfo>& segments, const core::Utc& finishedAt) {
    if (!segments.empty()) {
        return AppError{ErrorCode::InvalidArgument,
                        "segment persistence is not available in this database operation"};
    }
    auto begun = SqliteTransaction::beginImmediate(connection_);
    if (!begun.hasValue()) {
        return begun.error();
    }
    auto transaction = std::move(begun).value();
    auto current = session(sessionId);
    if (!current.hasValue()) {
        return AppError{ErrorCode::InvalidState,
                        "recording session is not active"};
    }
    if (current.value().state != PersistedSessionState::Recording) {
        return AppError{ErrorCode::InvalidState,
                        "recording session is not active"};
    }
    if (stoppedAt < current.value().startedAt) {
        return AppError{ErrorCode::InvalidArgument,
                        "recording stop time cannot precede its start time"};
    }

    auto prepared = connection_.prepare(
        "UPDATE recording_sessions SET state='COMPLETED', stopped_ns=?1, "
        "finished_at_utc=?2, failure_reason=NULL "
        "WHERE session_id=?3 AND project_id=?4 AND state='RECORDING'");
    if (!prepared.hasValue()) {
        return prepared.error();
    }
    auto statement = std::move(prepared).value();
    if (auto bound = statement.bindInt64(1, toNanoseconds(stoppedAt)); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(2, finishedAt.toRfc3339()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(3, sessionId.value()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(4, projectId_.value()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto updated = expectDone(statement, "complete recording"); !updated.hasValue()) {
        return updated.error();
    }
    if (connection_.changes() != 1) {
        return AppError{ErrorCode::InvalidState, "recording session is not active"};
    }
    if (auto committed = transaction.commit(); !committed.hasValue()) {
        return committed.error();
    }
    return core::ok();
}

Result<void> SqliteProjectDatabase::abortRecording(const SessionId& sessionId,
                                                   std::string_view reason,
                                                   const core::Utc& finishedAt) {
    auto prepared = connection_.prepare(
        "UPDATE recording_sessions "
        "SET state='ABORTED', stopped_ns=started_ns, finished_at_utc=?1, failure_reason=?2 "
        "WHERE session_id=?3 AND project_id=?4 AND state='RECORDING'");
    if (!prepared.hasValue()) {
        return prepared.error();
    }
    auto statement = std::move(prepared).value();
    if (auto bound = statement.bindText(1, finishedAt.toRfc3339()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(2, reason); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(3, sessionId.value()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(4, projectId_.value()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto updated = expectDone(statement, "abort recording"); !updated.hasValue()) {
        return updated.error();
    }
    if (connection_.changes() != 1) {
        return AppError{ErrorCode::InvalidState, "recording session is not active"};
    }
    return core::ok();
}

Result<RecordingSessionRecord> SqliteProjectDatabase::session(const SessionId& sessionId) {
    auto prepared = connection_.prepare(
        "SELECT state, started_ns, stopped_ns, created_at_utc, finished_at_utc, "
        "failure_reason FROM recording_sessions WHERE session_id=?1 AND project_id=?2");
    if (!prepared.hasValue()) {
        return prepared.error();
    }
    auto statement = std::move(prepared).value();
    if (auto bound = statement.bindText(1, sessionId.value()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(2, projectId_.value()); !bound.hasValue()) {
        return bound.error();
    }
    auto row = statement.step();
    if (!row.hasValue()) {
        return row.error();
    }
    if (row.value() != SqliteStep::Row) {
        return AppError{ErrorCode::NotFound, "recording session was not found"};
    }
    auto state = parseState(statement.columnText(0));
    if (!state.hasValue()) {
        return state.error();
    }
    const std::int64_t startedNs = statement.columnInt64(1);
    if (startedNs < 0) {
        return AppError{ErrorCode::IoFailure, "sqlite recording session has invalid start time"};
    }
    std::optional<TimestampNs> stoppedAt;
    if (!statement.columnIsNull(2)) {
        const std::int64_t stoppedNs = statement.columnInt64(2);
        if (stoppedNs < startedNs) {
            return AppError{ErrorCode::IoFailure, "sqlite recording session has invalid stop time"};
        }
        stoppedAt = fromNanoseconds(stoppedNs);
    }
    auto createdAt = parsePersistedUtc(statement.columnText(3), "creation time");
    if (!createdAt.hasValue()) {
        return createdAt.error();
    }
    std::optional<core::Utc> finishedAt;
    if (!statement.columnIsNull(4)) {
        auto parsed = parsePersistedUtc(statement.columnText(4), "finish time");
        if (!parsed.hasValue()) {
            return parsed.error();
        }
        finishedAt = parsed.value();
    }
    std::optional<std::string> failureReason;
    if (!statement.columnIsNull(5)) {
        failureReason = statement.columnText(5);
    }
    auto end = statement.step();
    if (!end.hasValue()) {
        return end.error();
    }
    if (end.value() != SqliteStep::Done) {
        return AppError{ErrorCode::IoFailure,
                        "sqlite returned duplicate recording session identities"};
    }
    return RecordingSessionRecord{
        .id = sessionId,
        .state = state.value(),
        .startedAt = fromNanoseconds(startedNs),
        .stoppedAt = stoppedAt,
        .createdAt = createdAt.value(),
        .finishedAt = finishedAt,
        .failureReason = failureReason,
    };
}

}  // namespace creator::project_store
