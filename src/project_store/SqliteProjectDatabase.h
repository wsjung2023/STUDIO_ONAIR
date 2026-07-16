#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "core/Utc.h"
#include "domain/Identifiers.h"
#include "domain/ProjectManifest.h"
#include "domain/Segment.h"
#include "project_store/ProjectPackage.h"
#include "project_store/internal/SqliteConnection.h"

#include <filesystem>
#include <string_view>
#include <vector>

namespace creator::project_store {

class SqliteProjectDatabase final {
public:
    [[nodiscard]] static core::Result<SqliteProjectDatabase> create(
        const std::filesystem::path& databasePath,
        const domain::ProjectManifest& manifest);
    [[nodiscard]] static core::Result<SqliteProjectDatabase> open(
        const std::filesystem::path& databasePath,
        const domain::ProjectId& expectedProjectId);

    SqliteProjectDatabase(SqliteProjectDatabase&&) noexcept = default;
    SqliteProjectDatabase& operator=(SqliteProjectDatabase&&) noexcept = default;
    SqliteProjectDatabase(const SqliteProjectDatabase&) = delete;
    SqliteProjectDatabase& operator=(const SqliteProjectDatabase&) = delete;

    [[nodiscard]] core::Result<void> beginRecording(
        const domain::SessionId& sessionId, core::TimestampNs startedAt,
        const core::Utc& createdAt);
    [[nodiscard]] core::Result<void> completeRecording(
        const domain::SessionId& sessionId, core::TimestampNs stoppedAt,
        const std::vector<domain::SegmentInfo>& segments, const core::Utc& finishedAt);
    [[nodiscard]] core::Result<void> abortRecording(
        const domain::SessionId& sessionId, std::string_view reason,
        const core::Utc& finishedAt);
    [[nodiscard]] core::Result<RecordingSessionRecord> session(
        const domain::SessionId& sessionId);

private:
    SqliteProjectDatabase(internal::SqliteConnection connection, domain::ProjectId projectId);
    internal::SqliteConnection connection_;
    domain::ProjectId projectId_;
};

}  // namespace creator::project_store
