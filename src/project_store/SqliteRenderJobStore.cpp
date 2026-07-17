#include "project_store/SqliteRenderJobStore.h"

#include "core/AppError.h"
#include "project_store/MigrationRunner.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace creator::project_store {
namespace {

using core::AppError;
using core::DurationNs;
using core::ErrorCode;
using core::Result;
using core::TimestampNs;
using edit_engine::RenderFallbackPolicy;
using edit_engine::RenderJobState;
using edit_engine::RenderOverwritePolicy;
using internal::SqliteStatement;
using internal::SqliteStep;
using internal::SqliteTransaction;

AppError invalid(std::string message) {
    return AppError{ErrorCode::InvalidArgument, std::move(message)};
}

AppError corrupt(std::string message) {
    return AppError{ErrorCode::IoFailure,
                    "sqlite render job is invalid: " + std::move(message)};
}

Result<void> expectDone(SqliteStatement& statement,
                        std::string_view operation) {
    auto stepped = statement.step();
    if (!stepped.hasValue()) return stepped.error();
    if (stepped.value() != SqliteStep::Done) {
        return corrupt(std::string{operation} + " unexpectedly returned a row");
    }
    return core::ok();
}

std::string_view stateName(RenderJobState state) noexcept {
    switch (state) {
        case RenderJobState::Pending: return "PENDING";
        case RenderJobState::Running: return "RUNNING";
        case RenderJobState::Publishing: return "PUBLISHING";
        case RenderJobState::Cancelling: return "CANCELLING";
        case RenderJobState::Completed: return "COMPLETED";
        case RenderJobState::Failed: return "FAILED";
        case RenderJobState::Cancelled: return "CANCELLED";
    }
    return {};
}

Result<RenderJobState> stateFromName(std::string_view value) {
    if (value == "PENDING") return RenderJobState::Pending;
    if (value == "RUNNING") return RenderJobState::Running;
    if (value == "PUBLISHING") return RenderJobState::Publishing;
    if (value == "CANCELLING") return RenderJobState::Cancelling;
    if (value == "COMPLETED") return RenderJobState::Completed;
    if (value == "FAILED") return RenderJobState::Failed;
    if (value == "CANCELLED") return RenderJobState::Cancelled;
    return corrupt("unknown state");
}

std::string_view fallbackName(RenderFallbackPolicy policy) noexcept {
    return policy == RenderFallbackPolicy::HardwareThenSoftware
               ? "HARDWARE_THEN_SOFTWARE"
               : "SOFTWARE_ONLY";
}

Result<RenderFallbackPolicy> fallbackFromName(std::string_view value) {
    if (value == "HARDWARE_THEN_SOFTWARE") {
        return RenderFallbackPolicy::HardwareThenSoftware;
    }
    if (value == "SOFTWARE_ONLY") return RenderFallbackPolicy::SoftwareOnly;
    return corrupt("unknown fallback policy");
}

std::string_view overwriteName(RenderOverwritePolicy policy) noexcept {
    return policy == RenderOverwritePolicy::FailIfExists
               ? "FAIL_IF_EXISTS"
               : "REPLACE_EXISTING";
}

Result<RenderOverwritePolicy> overwriteFromName(std::string_view value) {
    if (value == "FAIL_IF_EXISTS") return RenderOverwritePolicy::FailIfExists;
    if (value == "REPLACE_EXISTING") {
        return RenderOverwritePolicy::ReplaceExisting;
    }
    return corrupt("unknown overwrite policy");
}

Result<std::string> pathToUtf8(const std::filesystem::path& path) {
    try {
        const auto utf8 = path.generic_u8string();
        return std::string{reinterpret_cast<const char*>(utf8.data()),
                           utf8.size()};
    } catch (const std::system_error&) {
        return invalid("render job path is not valid UTF-8");
    }
}

Result<std::filesystem::path> pathFromUtf8(std::string_view value) {
    try {
        std::u8string utf8;
        utf8.reserve(value.size());
        for (const unsigned char byte : value) {
            utf8.push_back(static_cast<char8_t>(byte));
        }
        return std::filesystem::path{utf8};
    } catch (const std::system_error&) {
        return corrupt("path is not valid UTF-8");
    }
}

Result<void> bindOptionalText(SqliteStatement& statement, int index,
                              const std::optional<std::string>& value) {
    return value.has_value() ? statement.bindText(index, *value)
                             : statement.bindNull(index);
}

Result<void> bindOptionalUtc(SqliteStatement& statement, int index,
                             const std::optional<core::Utc>& value) {
    return value.has_value() ? statement.bindText(index, value->toRfc3339())
                             : statement.bindNull(index);
}

std::optional<std::string> optionalText(const SqliteStatement& statement,
                                        int column) {
    if (statement.columnIsNull(column)) return std::nullopt;
    return statement.columnText(column);
}

Result<std::optional<core::Utc>> optionalUtc(const SqliteStatement& statement,
                                             int column) {
    if (statement.columnIsNull(column)) return std::optional<core::Utc>{};
    auto parsed = core::Utc::parseRfc3339(statement.columnText(column));
    if (!parsed.hasValue()) return corrupt("timestamp is malformed");
    return std::optional<core::Utc>{std::move(parsed).value()};
}

Result<RenderJobRecord> readRecord(const SqliteStatement& statement) {
    auto jobId = domain::RenderJobId::create(statement.columnText(0));
    auto projectId = domain::ProjectId::create(statement.columnText(1));
    auto timelineId = domain::TimelineId::create(statement.columnText(2));
    auto revision = domain::TimelineRevision::create(statement.columnInt64(3));
    auto frameRate = core::FrameRate::create(statement.columnInt64(7),
                                             statement.columnInt64(8));
    auto fallback = fallbackFromName(statement.columnText(11));
    auto overwrite = overwriteFromName(statement.columnText(12));
    auto state = stateFromName(statement.columnText(15));
    const auto width = statement.columnInt64(5);
    const auto height = statement.columnInt64(6);
    const auto videoBitrate = statement.columnInt64(9);
    const auto audioBitrate = statement.columnInt64(10);
    if (!jobId.hasValue() || !projectId.hasValue() || !timelineId.hasValue() ||
        !revision.hasValue() || !frameRate.hasValue() || !fallback.hasValue() ||
        !overwrite.hasValue() || !state.hasValue() || width <= 0 || height <= 0 ||
        videoBitrate <= 0 || audioBitrate <= 0 ||
        width > std::numeric_limits<std::uint32_t>::max() ||
        height > std::numeric_limits<std::uint32_t>::max() ||
        videoBitrate > std::numeric_limits<std::uint32_t>::max() ||
        audioBitrate > std::numeric_limits<std::uint32_t>::max()) {
        return corrupt("identity or preset values are malformed");
    }
    auto preset = edit_engine::RenderPreset::create(
        statement.columnText(4), static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height), frameRate.value(),
        static_cast<std::uint32_t>(videoBitrate),
        static_cast<std::uint32_t>(audioBitrate), fallback.value());
    auto progress = edit_engine::RenderProgress::create(
        state.value(), statement.columnDouble(18),
        TimestampNs{DurationNs{statement.columnInt64(16)}},
        DurationNs{statement.columnInt64(17)});
    auto createdAt = core::Utc::parseRfc3339(statement.columnText(25));
    auto startedAt = optionalUtc(statement, 26);
    auto updatedAt = core::Utc::parseRfc3339(statement.columnText(27));
    auto finishedAt = optionalUtc(statement, 28);
    auto destination = pathFromUtf8(statement.columnText(13));
    auto partial = pathFromUtf8(statement.columnText(14));
    if (!preset.hasValue() || !progress.hasValue() || !createdAt.hasValue() ||
        !startedAt.hasValue() || !updatedAt.hasValue() ||
        !finishedAt.hasValue() || !destination.hasValue() ||
        !partial.hasValue()) {
        return corrupt("preset, progress, or timestamps are malformed");
    }
    return RenderJobRecord{
        .jobId = std::move(jobId).value(),
        .projectId = std::move(projectId).value(),
        .timelineId = std::move(timelineId).value(),
        .timelineRevision = revision.value(),
        .preset = std::move(preset).value(),
        .overwritePolicy = overwrite.value(),
        .destination = std::move(destination).value(),
        .partial = std::move(partial).value(),
        .progress = std::move(progress).value(),
        .diagnostics = RenderJobDiagnostics{
            .attemptedEncoder = optionalText(statement, 19),
            .selectedEncoder = optionalText(statement, 20),
            .fallbackReason = optionalText(statement, 21),
            .diagnostic = optionalText(statement, 22),
            .outputSha256 = optionalText(statement, 23),
            .destinationIdentity = optionalText(statement, 24)},
        .createdAt = std::move(createdAt).value(),
        .startedAt = std::move(startedAt).value(),
        .updatedAt = std::move(updatedAt).value(),
        .finishedAt = std::move(finishedAt).value()};
}

Result<void> bindDiagnostics(SqliteStatement& statement, int first,
                             const RenderJobDiagnostics& diagnostics) {
    const std::optional<std::string>* values[]{
        &diagnostics.attemptedEncoder, &diagnostics.selectedEncoder,
        &diagnostics.fallbackReason, &diagnostics.diagnostic,
        &diagnostics.outputSha256, &diagnostics.destinationIdentity};
    for (int index = 0; index < 6; ++index) {
        if (auto bound = bindOptionalText(statement, first + index,
                                          *values[index]);
            !bound.hasValue()) {
            return bound.error();
        }
    }
    return core::ok();
}

}  // namespace

Result<SqliteRenderJobStore> SqliteRenderJobStore::open(
    const std::filesystem::path& databasePath,
    const domain::ProjectId& expectedProjectId,
    internal::SqliteConnection::IdentityVerifier identityVerifier) {
    auto opened = internal::SqliteConnection::open(databasePath,
                                                    identityVerifier);
    if (!opened.hasValue()) return opened.error();
    auto connection = std::move(opened).value();
    if (auto migrated = MigrationRunner::apply(connection);
        !migrated.hasValue()) {
        return migrated.error();
    }
    if (identityVerifier) {
        auto identity = identityVerifier();
        if (!identity.hasValue()) return identity.error();
    }
    auto query = connection.prepare(
        "SELECT count(*) FROM projects WHERE project_id=?1");
    if (!query.hasValue()) return query.error();
    auto statement = std::move(query).value();
    if (auto bound = statement.bindText(1, expectedProjectId.value());
        !bound.hasValue()) return bound.error();
    auto row = statement.step();
    if (!row.hasValue()) return row.error();
    if (row.value() != SqliteStep::Row || statement.columnInt64(0) != 1) {
        return AppError{ErrorCode::NotFound,
                        "render job project was not found"};
    }
    return SqliteRenderJobStore{std::move(connection), expectedProjectId};
}

Result<void> SqliteRenderJobStore::begin(const RenderJobRecord& pending) {
    if (pending.projectId != projectId_ ||
        pending.progress.state() != RenderJobState::Pending ||
        pending.startedAt.has_value() || pending.finishedAt.has_value() ||
        !pending.destination.is_absolute() || !pending.partial.is_absolute() ||
        pending.destination == pending.partial) {
        return invalid("pending render job values are invalid");
    }
    auto begun = SqliteTransaction::beginImmediate(connection_);
    if (!begun.hasValue()) return begun.error();
    auto transaction = std::move(begun).value();
    auto existing = load(pending.jobId);
    if (!existing.hasValue()) return existing.error();
    if (existing.value().has_value()) {
        if (*existing.value() == pending) return transaction.commit();
        return AppError{ErrorCode::InvalidState,
                        "render job id already has different values"};
    }
    auto timeline = connection_.prepare(
        "SELECT revision FROM timelines WHERE timeline_id=?1 AND project_id=?2");
    if (!timeline.hasValue()) return timeline.error();
    auto timelineStatement = std::move(timeline).value();
    if (auto bound = timelineStatement.bindText(1, pending.timelineId.value());
        !bound.hasValue()) return bound.error();
    if (auto bound = timelineStatement.bindText(2, projectId_.value());
        !bound.hasValue()) return bound.error();
    auto timelineRow = timelineStatement.step();
    if (!timelineRow.hasValue()) return timelineRow.error();
    if (timelineRow.value() != SqliteStep::Row ||
        timelineStatement.columnInt64(0) != pending.timelineRevision.value()) {
        return AppError{ErrorCode::InvalidState,
                        "render job timeline revision is not current"};
    }
    auto destination = pathToUtf8(pending.destination);
    auto partial = pathToUtf8(pending.partial);
    if (!destination.hasValue()) return destination.error();
    if (!partial.hasValue()) return partial.error();
    auto inserted = connection_.prepare(
        "INSERT INTO render_jobs VALUES("
        "?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,"
        "?16,?17,?18,?19,?20,?21,?22,?23,?24,?25,?26,?27,?28,?29)");
    if (!inserted.hasValue()) return inserted.error();
    auto statement = std::move(inserted).value();
    if (auto value = statement.bindText(1, pending.jobId.value()); !value.hasValue()) return value.error();
    if (auto value = statement.bindText(2, projectId_.value()); !value.hasValue()) return value.error();
    if (auto value = statement.bindText(3, pending.timelineId.value()); !value.hasValue()) return value.error();
    if (auto value = statement.bindInt64(4, pending.timelineRevision.value()); !value.hasValue()) return value.error();
    if (auto value = statement.bindText(5, pending.preset.id()); !value.hasValue()) return value.error();
    if (auto value = statement.bindInt64(6, pending.preset.width()); !value.hasValue()) return value.error();
    if (auto value = statement.bindInt64(7, pending.preset.height()); !value.hasValue()) return value.error();
    if (auto value = statement.bindInt64(8, pending.preset.frameRate().numerator()); !value.hasValue()) return value.error();
    if (auto value = statement.bindInt64(9, pending.preset.frameRate().denominator()); !value.hasValue()) return value.error();
    if (auto value = statement.bindInt64(10, pending.preset.videoBitrate()); !value.hasValue()) return value.error();
    if (auto value = statement.bindInt64(11, pending.preset.audioBitrate()); !value.hasValue()) return value.error();
    if (auto value = statement.bindText(12, fallbackName(pending.preset.fallbackPolicy())); !value.hasValue()) return value.error();
    if (auto value = statement.bindText(13, overwriteName(pending.overwritePolicy)); !value.hasValue()) return value.error();
    if (auto value = statement.bindText(14, destination.value()); !value.hasValue()) return value.error();
    if (auto value = statement.bindText(15, partial.value()); !value.hasValue()) return value.error();
    if (auto value = statement.bindText(16, stateName(pending.progress.state())); !value.hasValue()) return value.error();
    if (auto value = statement.bindInt64(17, pending.progress.renderedThrough().time_since_epoch().count()); !value.hasValue()) return value.error();
    if (auto value = statement.bindInt64(18, pending.progress.totalDuration().count()); !value.hasValue()) return value.error();
    if (auto value = statement.bindDouble(19, pending.progress.fraction()); !value.hasValue()) return value.error();
    if (auto value = bindDiagnostics(statement, 20, pending.diagnostics); !value.hasValue()) return value.error();
    if (auto value = statement.bindText(26, pending.createdAt.toRfc3339()); !value.hasValue()) return value.error();
    if (auto value = bindOptionalUtc(statement, 27, pending.startedAt); !value.hasValue()) return value.error();
    if (auto value = statement.bindText(28, pending.updatedAt.toRfc3339()); !value.hasValue()) return value.error();
    if (auto value = bindOptionalUtc(statement, 29, pending.finishedAt); !value.hasValue()) return value.error();
    if (auto done = expectDone(statement, "render job insert"); !done.hasValue()) return done.error();
    return transaction.commit();
}

Result<void> SqliteRenderJobStore::advance(
    const domain::RenderJobId& jobId, const RenderJobUpdate& update) {
    auto begun = SqliteTransaction::beginImmediate(connection_);
    if (!begun.hasValue()) return begun.error();
    auto transaction = std::move(begun).value();
    auto current = load(jobId);
    if (!current.hasValue()) return current.error();
    if (!current.value().has_value()) {
        return AppError{ErrorCode::NotFound, "render job was not found"};
    }
    const auto& previous = *current.value();
    RenderJobRecord next = previous;
    next.progress = update.progress;
    next.diagnostics = update.diagnostics;
    next.startedAt = update.startedAt;
    next.updatedAt = update.updatedAt;
    next.finishedAt = update.finishedAt;
    if (next == previous) return transaction.commit();
    if (auto valid = edit_engine::validateRenderProgressTransition(
            previous.progress, next.progress);
        !valid.hasValue()) {
        return valid.error();
    }
    if (next.updatedAt < previous.updatedAt ||
        (previous.startedAt.has_value() &&
         next.startedAt != previous.startedAt) ||
        (previous.finishedAt.has_value() &&
         next.finishedAt != previous.finishedAt)) {
        return invalid("render job timestamps must be monotonic and immutable");
    }
    auto changed = connection_.prepare(
        "UPDATE render_jobs SET state=?1,rendered_through_ns=?2,fraction=?3,"
        "attempted_encoder=?4,selected_encoder=?5,fallback_reason=?6,"
        "diagnostic=?7,output_sha256=?8,destination_identity=?9,"
        "started_at_utc=?10,updated_at_utc=?11,finished_at_utc=?12 "
        "WHERE job_id=?13 AND project_id=?14");
    if (!changed.hasValue()) return changed.error();
    auto statement = std::move(changed).value();
    if (auto value = statement.bindText(1, stateName(next.progress.state())); !value.hasValue()) return value.error();
    if (auto value = statement.bindInt64(2, next.progress.renderedThrough().time_since_epoch().count()); !value.hasValue()) return value.error();
    if (auto value = statement.bindDouble(3, next.progress.fraction()); !value.hasValue()) return value.error();
    if (auto value = bindDiagnostics(statement, 4, next.diagnostics); !value.hasValue()) return value.error();
    if (auto value = bindOptionalUtc(statement, 10, next.startedAt); !value.hasValue()) return value.error();
    if (auto value = statement.bindText(11, next.updatedAt.toRfc3339()); !value.hasValue()) return value.error();
    if (auto value = bindOptionalUtc(statement, 12, next.finishedAt); !value.hasValue()) return value.error();
    if (auto value = statement.bindText(13, jobId.value()); !value.hasValue()) return value.error();
    if (auto value = statement.bindText(14, projectId_.value()); !value.hasValue()) return value.error();
    if (auto done = expectDone(statement, "render job update"); !done.hasValue()) return done.error();
    if (connection_.changes() != 1) {
        return AppError{ErrorCode::NotFound, "render job update matched no row"};
    }
    return transaction.commit();
}

Result<std::optional<RenderJobRecord>> SqliteRenderJobStore::load(
    const domain::RenderJobId& jobId) {
    auto query = connection_.prepare(
        "SELECT job_id,project_id,timeline_id,timeline_revision,preset_id,"
        "width,height,frame_rate_numerator,frame_rate_denominator,video_bitrate,"
        "audio_bitrate,fallback_policy,overwrite_policy,destination_path,"
        "partial_path,state,rendered_through_ns,total_duration_ns,fraction,"
        "attempted_encoder,selected_encoder,fallback_reason,diagnostic,"
        "output_sha256,destination_identity,created_at_utc,started_at_utc,"
        "updated_at_utc,finished_at_utc FROM render_jobs "
        "WHERE job_id=?1 AND project_id=?2");
    if (!query.hasValue()) return query.error();
    auto statement = std::move(query).value();
    if (auto bound = statement.bindText(1, jobId.value()); !bound.hasValue()) return bound.error();
    if (auto bound = statement.bindText(2, projectId_.value()); !bound.hasValue()) return bound.error();
    auto row = statement.step();
    if (!row.hasValue()) return row.error();
    if (row.value() == SqliteStep::Done) {
        return std::optional<RenderJobRecord>{};
    }
    auto record = readRecord(statement);
    if (!record.hasValue()) return record.error();
    auto end = statement.step();
    if (!end.hasValue()) return end.error();
    if (end.value() != SqliteStep::Done) return corrupt("duplicate job id");
    return std::optional<RenderJobRecord>{std::move(record).value()};
}

Result<std::vector<RenderJobRecord>> SqliteRenderJobStore::listRecoverable() {
    auto query = connection_.prepare(
        "SELECT job_id FROM render_jobs WHERE project_id=?1 AND state IN "
        "('PENDING','RUNNING','PUBLISHING','CANCELLING') "
        "ORDER BY created_at_utc,job_id");
    if (!query.hasValue()) return query.error();
    auto statement = std::move(query).value();
    if (auto bound = statement.bindText(1, projectId_.value()); !bound.hasValue()) return bound.error();
    std::vector<domain::RenderJobId> ids;
    while (true) {
        auto row = statement.step();
        if (!row.hasValue()) return row.error();
        if (row.value() == SqliteStep::Done) break;
        auto id = domain::RenderJobId::create(statement.columnText(0));
        if (!id.hasValue()) return corrupt("recoverable job id is empty");
        ids.push_back(std::move(id).value());
    }
    std::vector<RenderJobRecord> records;
    records.reserve(ids.size());
    for (const auto& id : ids) {
        auto loaded = load(id);
        if (!loaded.hasValue()) return loaded.error();
        if (!loaded.value().has_value()) {
            return corrupt("recoverable job disappeared");
        }
        records.push_back(std::move(*loaded.value()));
    }
    return records;
}

}  // namespace creator::project_store
