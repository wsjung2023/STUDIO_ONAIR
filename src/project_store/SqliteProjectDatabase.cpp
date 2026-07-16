#include "project_store/SqliteProjectDatabase.h"

#include "core/AppError.h"
#include "project_store/MigrationRunner.h"

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace creator::project_store {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;
using core::TimestampNs;
using domain::ProjectId;
using domain::ProjectManifest;
using domain::SegmentInfo;
using domain::SegmentStatus;
using domain::SessionId;
using domain::SourceId;
using internal::SqliteConnection;
using internal::SqliteStep;
using internal::SqliteTransaction;

namespace fs = std::filesystem;

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

Result<std::string> normalizeRelativePath(std::string_view value) {
    if (value.empty() || value.find('\0') != std::string_view::npos) {
        return AppError{ErrorCode::InvalidArgument, "segment path must be a non-empty relative path"};
    }

    std::string portable{value};
    std::replace(portable.begin(), portable.end(), '\\', '/');
    const bool driveRoot = portable.size() >= 2 &&
                           std::isalpha(static_cast<unsigned char>(portable[0])) != 0 &&
                           portable[1] == ':';
    if (portable.front() == '/' || driveRoot) {
        return AppError{ErrorCode::InvalidArgument, "segment path must be relative"};
    }

    try {
        std::u8string utf8;
        utf8.reserve(portable.size());
        for (const unsigned char byte : portable) {
            utf8.push_back(static_cast<char8_t>(byte));
        }
        const fs::path normalized = fs::path{utf8}.lexically_normal();
        if (normalized.empty() || normalized == fs::path{"."} || normalized.is_absolute() ||
            normalized.has_root_directory() || normalized.has_root_name()) {
            return AppError{ErrorCode::InvalidArgument,
                            "segment path must resolve inside the package"};
        }
        for (const auto& component : normalized) {
            if (component == fs::path{".."}) {
                return AppError{ErrorCode::InvalidArgument,
                                "segment path must not escape the package"};
            }
        }
        const std::u8string generic = normalized.generic_u8string();
        return std::string{reinterpret_cast<const char*>(generic.data()), generic.size()};
    } catch (const std::system_error&) {
        return AppError{ErrorCode::InvalidArgument, "segment path is not valid UTF-8"};
    }
}

struct ValidatedSegment final {
    std::int64_t index;
    std::int64_t startNs;
    std::int64_t durationNs;
    std::string relativePath;
};

Result<ValidatedSegment> validateSegment(const SegmentInfo& segment,
                                         SegmentStatus requiredStatus) {
    if (segment.status != requiredStatus) {
        return AppError{ErrorCode::InvalidArgument, "segment has the wrong lifecycle state"};
    }
    if (segment.index > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return AppError{ErrorCode::InvalidArgument, "segment index exceeds SQLite int64 range"};
    }
    const std::int64_t startNs = toNanoseconds(segment.startTime);
    const std::int64_t durationNs = segment.duration.count();
    if (startNs < 0 || (requiredStatus == SegmentStatus::Ready && durationNs < 0)) {
        return AppError{ErrorCode::InvalidArgument, "segment time values cannot be negative"};
    }
    auto path = normalizeRelativePath(segment.relativePath);
    if (!path.hasValue()) {
        return path.error();
    }
    return ValidatedSegment{.index = static_cast<std::int64_t>(segment.index),
                            .startNs = startNs,
                            .durationNs = durationNs,
                            .relativePath = std::move(path).value()};
}

struct PersistedSegment final {
    std::int64_t startNs;
    std::optional<std::int64_t> durationNs;
    std::string status;
    std::string relativePath;
};

Result<std::optional<PersistedSegment>> loadSegment(SqliteConnection& connection,
                                                    const SessionId& sessionId,
                                                    const SourceId& sourceId,
                                                    std::int64_t segmentIndex) {
    auto prepared = connection.prepare(
        "SELECT start_ns, duration_ns, status, relative_path FROM segments "
        "WHERE session_id=?1 AND source_id=?2 AND segment_index=?3");
    if (!prepared.hasValue()) return prepared.error();
    auto statement = std::move(prepared).value();
    if (auto bound = statement.bindText(1, sessionId.value()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(2, sourceId.value()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindInt64(3, segmentIndex); !bound.hasValue()) {
        return bound.error();
    }
    auto row = statement.step();
    if (!row.hasValue()) return row.error();
    if (row.value() == SqliteStep::Done) {
        return std::optional<PersistedSegment>{};
    }
    PersistedSegment result{
        .startNs = statement.columnInt64(0),
        .durationNs = statement.columnIsNull(1)
                          ? std::optional<std::int64_t>{}
                          : std::optional<std::int64_t>{statement.columnInt64(1)},
        .status = statement.columnText(2),
        .relativePath = statement.columnText(3),
    };
    auto end = statement.step();
    if (!end.hasValue()) return end.error();
    if (end.value() != SqliteStep::Done) {
        return AppError{ErrorCode::IoFailure, "sqlite returned duplicate segment identities"};
    }
    return std::optional<PersistedSegment>{std::move(result)};
}

Result<void> insertSegment(SqliteConnection& connection, const SessionId& sessionId,
                           const SourceId& sourceId, const ValidatedSegment& segment,
                           std::string_view status,
                           std::optional<std::int64_t> durationNs) {
    auto prepared = connection.prepare(
        "INSERT INTO segments(session_id, source_id, segment_index, start_ns, duration_ns, "
        "status, relative_path) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");
    if (!prepared.hasValue()) return prepared.error();
    auto statement = std::move(prepared).value();
    if (auto bound = statement.bindText(1, sessionId.value()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(2, sourceId.value()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindInt64(3, segment.index); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindInt64(4, segment.startNs); !bound.hasValue()) {
        return bound.error();
    }
    if (durationNs.has_value()) {
        if (auto bound = statement.bindInt64(5, *durationNs); !bound.hasValue()) {
            return bound.error();
        }
    } else if (auto bound = statement.bindNull(5); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(6, status); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(7, segment.relativePath); !bound.hasValue()) {
        return bound.error();
    }
    return expectDone(statement, "insert segment");
}

bool matchesWriting(const PersistedSegment& stored, const ValidatedSegment& candidate) {
    return stored.status == "WRITING" && stored.startNs == candidate.startNs &&
           !stored.durationNs.has_value() && stored.relativePath == candidate.relativePath;
}

bool matchesReady(const PersistedSegment& stored, const ValidatedSegment& candidate) {
    return stored.status == "READY" && stored.startNs == candidate.startNs &&
           stored.durationNs == candidate.durationNs &&
           stored.relativePath == candidate.relativePath;
}

struct SegmentCounts final {
    std::size_t ready;
    std::size_t failed;
};

Result<SegmentCounts> segmentCounts(SqliteConnection& connection, const SessionId& sessionId) {
    auto prepared = connection.prepare(
        "SELECT SUM(CASE WHEN status='READY' THEN 1 ELSE 0 END), "
        "SUM(CASE WHEN status='FAILED' THEN 1 ELSE 0 END) "
        "FROM segments WHERE session_id=?1");
    if (!prepared.hasValue()) return prepared.error();
    auto statement = std::move(prepared).value();
    if (auto bound = statement.bindText(1, sessionId.value()); !bound.hasValue()) {
        return bound.error();
    }
    auto row = statement.step();
    if (!row.hasValue()) return row.error();
    if (row.value() != SqliteStep::Row) {
        return AppError{ErrorCode::IoFailure, "sqlite segment count query returned no row"};
    }
    const std::int64_t ready = statement.columnInt64(0);
    const std::int64_t failed = statement.columnInt64(1);
    if (ready < 0 || failed < 0 ||
        static_cast<std::uint64_t>(ready) > std::numeric_limits<std::size_t>::max() ||
        static_cast<std::uint64_t>(failed) > std::numeric_limits<std::size_t>::max()) {
        return AppError{ErrorCode::IoFailure, "sqlite segment counts are out of range"};
    }
    return SegmentCounts{.ready = static_cast<std::size_t>(ready),
                         .failed = static_cast<std::size_t>(failed)};
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
    for (const auto& segment : segments) {
        if (auto stored = storeCompletedSegment(sessionId, segment); !stored.hasValue()) {
            return stored.error();
        }
    }

    auto pendingPrepared = connection_.prepare(
        "SELECT count(*) FROM segments WHERE session_id=?1 AND status='WRITING'");
    if (!pendingPrepared.hasValue()) return pendingPrepared.error();
    auto pending = std::move(pendingPrepared).value();
    if (auto bound = pending.bindText(1, sessionId.value()); !bound.hasValue()) {
        return bound.error();
    }
    auto pendingRow = pending.step();
    if (!pendingRow.hasValue()) return pendingRow.error();
    if (pendingRow.value() != SqliteStep::Row) {
        return AppError{ErrorCode::IoFailure,
                        "sqlite pending segment query returned no row"};
    }
    if (pending.columnInt64(0) != 0) {
        return AppError{ErrorCode::InvalidState,
                        "recording session still has writing segments"};
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

Result<void> SqliteProjectDatabase::beginSegment(const SessionId& sessionId,
                                                 const SegmentInfo& segment) {
    auto validated = validateSegment(segment, SegmentStatus::Writing);
    if (!validated.hasValue()) return validated.error();
    auto active = session(sessionId);
    if (!active.hasValue() || active.value().state != PersistedSessionState::Recording) {
        return AppError{ErrorCode::InvalidState, "segments require an active recording session"};
    }
    auto existing = loadSegment(connection_, sessionId, segment.sourceId, validated.value().index);
    if (!existing.hasValue()) return existing.error();
    if (!existing.value().has_value()) {
        return insertSegment(connection_, sessionId, segment.sourceId, validated.value(),
                             "WRITING", std::nullopt);
    }
    if (matchesWriting(*existing.value(), validated.value())) {
        return core::ok();
    }
    return AppError{ErrorCode::InvalidState,
                    "segment identity already has different or terminal metadata"};
}

Result<void> SqliteProjectDatabase::markSegmentReady(const SessionId& sessionId,
                                                     const SegmentInfo& segment) {
    auto validated = validateSegment(segment, SegmentStatus::Ready);
    if (!validated.hasValue()) return validated.error();
    auto existing = loadSegment(connection_, sessionId, segment.sourceId, validated.value().index);
    if (!existing.hasValue()) return existing.error();
    if (!existing.value().has_value()) {
        return AppError{ErrorCode::InvalidState, "segment was not opened for writing"};
    }
    if (matchesReady(*existing.value(), validated.value())) {
        return core::ok();
    }
    auto active = session(sessionId);
    if (!active.hasValue() || active.value().state != PersistedSessionState::Recording) {
        return AppError{ErrorCode::InvalidState, "segments require an active recording session"};
    }
    if (!matchesWriting(*existing.value(), validated.value())) {
        return AppError{ErrorCode::InvalidState,
                        "segment cannot be rewritten after reaching a terminal state"};
    }

    auto prepared = connection_.prepare(
        "UPDATE segments SET start_ns=?1, duration_ns=?2, status='READY', relative_path=?3 "
        "WHERE session_id=?4 AND source_id=?5 AND segment_index=?6 AND status='WRITING'");
    if (!prepared.hasValue()) return prepared.error();
    auto statement = std::move(prepared).value();
    if (auto bound = statement.bindInt64(1, validated.value().startNs); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindInt64(2, validated.value().durationNs); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(3, validated.value().relativePath); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(4, sessionId.value()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(5, segment.sourceId.value()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindInt64(6, validated.value().index); !bound.hasValue()) {
        return bound.error();
    }
    if (auto updated = expectDone(statement, "mark segment ready"); !updated.hasValue()) {
        return updated.error();
    }
    if (connection_.changes() != 1) {
        return AppError{ErrorCode::InvalidState, "segment is no longer writable"};
    }
    return core::ok();
}

Result<void> SqliteProjectDatabase::markSegmentFailed(const SessionId& sessionId,
                                                      const SourceId& sourceId,
                                                      std::uint64_t segmentIndex) {
    if (segmentIndex > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return AppError{ErrorCode::InvalidArgument, "segment index exceeds SQLite int64 range"};
    }
    const auto index = static_cast<std::int64_t>(segmentIndex);
    auto existing = loadSegment(connection_, sessionId, sourceId, index);
    if (!existing.hasValue()) return existing.error();
    if (!existing.value().has_value()) {
        return AppError{ErrorCode::InvalidState, "segment was not opened for writing"};
    }
    if (existing.value()->status == "FAILED") return core::ok();
    auto active = session(sessionId);
    if (!active.hasValue() || active.value().state != PersistedSessionState::Recording) {
        return AppError{ErrorCode::InvalidState, "segments require an active recording session"};
    }
    if (existing.value()->status != "WRITING") {
        return AppError{ErrorCode::InvalidState,
                        "ready segments cannot be changed to failed"};
    }
    auto prepared = connection_.prepare(
        "UPDATE segments SET status='FAILED' WHERE session_id=?1 AND source_id=?2 "
        "AND segment_index=?3 AND status='WRITING'");
    if (!prepared.hasValue()) return prepared.error();
    auto statement = std::move(prepared).value();
    if (auto bound = statement.bindText(1, sessionId.value()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindText(2, sourceId.value()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = statement.bindInt64(3, index); !bound.hasValue()) {
        return bound.error();
    }
    if (auto updated = expectDone(statement, "mark segment failed"); !updated.hasValue()) {
        return updated.error();
    }
    if (connection_.changes() != 1) {
        return AppError{ErrorCode::InvalidState, "segment is no longer writable"};
    }
    return core::ok();
}

Result<void> SqliteProjectDatabase::storeCompletedSegment(const SessionId& sessionId,
                                                          const SegmentInfo& segment) {
    auto validated = validateSegment(segment, SegmentStatus::Ready);
    if (!validated.hasValue()) return validated.error();
    auto existing = loadSegment(connection_, sessionId, segment.sourceId, validated.value().index);
    if (!existing.hasValue()) return existing.error();
    if (!existing.value().has_value()) {
        return insertSegment(connection_, sessionId, segment.sourceId, validated.value(), "READY",
                             validated.value().durationNs);
    }
    if (matchesReady(*existing.value(), validated.value())) return core::ok();
    if (matchesWriting(*existing.value(), validated.value())) {
        return markSegmentReady(sessionId, segment);
    }
    return AppError{ErrorCode::InvalidState,
                    "completed segment conflicts with terminal metadata"};
}

Result<std::vector<RecoveryCandidate>> SqliteProjectDatabase::scanRecovery(
    const fs::path& packagePath, std::string_view projectName) {
    auto prepared = connection_.prepare(
        "SELECT r.session_id, r.created_at_utc, "
        "SUM(CASE WHEN s.status='READY' THEN 1 ELSE 0 END), "
        "SUM(CASE WHEN s.status='WRITING' THEN 1 ELSE 0 END) "
        "FROM recording_sessions r LEFT JOIN segments s ON s.session_id=r.session_id "
        "WHERE r.project_id=?1 AND r.state='RECORDING' "
        "GROUP BY r.session_id, r.created_at_utc ORDER BY r.created_at_utc, r.session_id");
    if (!prepared.hasValue()) return prepared.error();
    auto statement = std::move(prepared).value();
    if (auto bound = statement.bindText(1, projectId_.value()); !bound.hasValue()) {
        return bound.error();
    }

    std::vector<RecoveryCandidate> candidates;
    while (true) {
        auto row = statement.step();
        if (!row.hasValue()) return row.error();
        if (row.value() == SqliteStep::Done) break;
        auto sessionId = SessionId::create(statement.columnText(0));
        if (!sessionId.hasValue()) {
            return AppError{ErrorCode::IoFailure,
                            "sqlite recovery candidate has invalid session identity"};
        }
        auto createdAt = parsePersistedUtc(statement.columnText(1), "creation time");
        if (!createdAt.hasValue()) return createdAt.error();
        const std::int64_t ready = statement.columnInt64(2);
        const std::int64_t writing = statement.columnInt64(3);
        if (ready < 0 || writing < 0 ||
            static_cast<std::uint64_t>(ready) > std::numeric_limits<std::size_t>::max() ||
            static_cast<std::uint64_t>(writing) > std::numeric_limits<std::size_t>::max()) {
            return AppError{ErrorCode::IoFailure,
                            "sqlite recovery segment counts are out of range"};
        }
        candidates.push_back(RecoveryCandidate{
            .packagePath = packagePath,
            .projectName = std::string{projectName},
            .sessionId = std::move(sessionId).value(),
            .createdAt = createdAt.value(),
            .readySegments = static_cast<std::size_t>(ready),
            .writingSegments = static_cast<std::size_t>(writing),
        });
    }
    return candidates;
}

Result<RecoveryResult> SqliteProjectDatabase::recover(const SessionId& sessionId,
                                                      const core::Utc& finishedAt) {
    auto begun = SqliteTransaction::beginImmediate(connection_);
    if (!begun.hasValue()) return begun.error();
    auto transaction = std::move(begun).value();
    auto record = session(sessionId);
    if (!record.hasValue()) return record.error();

    if (record.value().state == PersistedSessionState::Recovered) {
        if (!record.value().stoppedAt.has_value()) {
            return AppError{ErrorCode::IoFailure,
                            "recovered session is missing its stop time"};
        }
        auto counts = segmentCounts(connection_, sessionId);
        if (!counts.hasValue()) return counts.error();
        if (auto committed = transaction.commit(); !committed.hasValue()) {
            return committed.error();
        }
        return RecoveryResult{.sessionId = sessionId,
                              .stoppedAt = *record.value().stoppedAt,
                              .readySegments = counts.value().ready,
                              .failedSegments = counts.value().failed};
    }
    if (record.value().state != PersistedSessionState::Recording) {
        return AppError{ErrorCode::InvalidState,
                        "only recording or recovered sessions can be recovered"};
    }

    std::int64_t stoppedNs = toNanoseconds(record.value().startedAt);
    auto readyQuery = connection_.prepare(
        "SELECT start_ns, duration_ns FROM segments "
        "WHERE session_id=?1 AND status='READY'");
    if (!readyQuery.hasValue()) return readyQuery.error();
    auto readyRows = std::move(readyQuery).value();
    if (auto bound = readyRows.bindText(1, sessionId.value()); !bound.hasValue()) {
        return bound.error();
    }
    while (true) {
        auto row = readyRows.step();
        if (!row.hasValue()) return row.error();
        if (row.value() == SqliteStep::Done) break;
        if (readyRows.columnIsNull(1)) {
            return AppError{ErrorCode::IoFailure,
                            "ready segment is missing its duration"};
        }
        const std::int64_t start = readyRows.columnInt64(0);
        const std::int64_t duration = readyRows.columnInt64(1);
        if (start < 0 || duration < 0) {
            return AppError{ErrorCode::IoFailure,
                            "ready segment has a negative time value"};
        }
        if (duration > std::numeric_limits<std::int64_t>::max() - start) {
            return AppError{ErrorCode::InvalidArgument,
                            "segment end timestamp overflows int64"};
        }
        stoppedNs = std::max(stoppedNs, start + duration);
    }

    auto failWriting = connection_.prepare(
        "UPDATE segments SET status='FAILED' WHERE session_id=?1 AND status='WRITING'");
    if (!failWriting.hasValue()) return failWriting.error();
    auto failStatement = std::move(failWriting).value();
    if (auto bound = failStatement.bindText(1, sessionId.value()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto failed = expectDone(failStatement, "fail interrupted segments");
        !failed.hasValue()) {
        return failed.error();
    }

    auto updateSession = connection_.prepare(
        "UPDATE recording_sessions SET state='RECOVERED', stopped_ns=?1, "
        "finished_at_utc=?2, failure_reason=NULL "
        "WHERE session_id=?3 AND project_id=?4 AND state='RECORDING'");
    if (!updateSession.hasValue()) return updateSession.error();
    auto update = std::move(updateSession).value();
    if (auto bound = update.bindInt64(1, stoppedNs); !bound.hasValue()) return bound.error();
    if (auto bound = update.bindText(2, finishedAt.toRfc3339()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = update.bindText(3, sessionId.value()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto bound = update.bindText(4, projectId_.value()); !bound.hasValue()) {
        return bound.error();
    }
    if (auto updated = expectDone(update, "recover recording session"); !updated.hasValue()) {
        return updated.error();
    }
    if (connection_.changes() != 1) {
        return AppError{ErrorCode::InvalidState,
                        "recording session changed while recovery was running"};
    }
    auto counts = segmentCounts(connection_, sessionId);
    if (!counts.hasValue()) return counts.error();
    if (auto committed = transaction.commit(); !committed.hasValue()) {
        return committed.error();
    }
    return RecoveryResult{.sessionId = sessionId,
                          .stoppedAt = fromNanoseconds(stoppedNs),
                          .readySegments = counts.value().ready,
                          .failedSegments = counts.value().failed};
}

}  // namespace creator::project_store
