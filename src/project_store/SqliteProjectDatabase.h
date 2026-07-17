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
#include <cstdint>
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
        const domain::ProjectId& expectedProjectId,
        internal::SqliteConnection::IdentityVerifier identityVerifier = {});

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
    [[nodiscard]] core::Result<std::vector<domain::SegmentInfo>> segments(
        const domain::SessionId& sessionId);
    [[nodiscard]] core::Result<void> beginSegment(
        const domain::SessionId& sessionId, const domain::SegmentInfo& segment);
    [[nodiscard]] core::Result<void> markSegmentReady(
        const domain::SessionId& sessionId, const domain::SegmentInfo& segment);
    [[nodiscard]] core::Result<void> markSegmentFailed(
        const domain::SessionId& sessionId, const domain::SourceId& sourceId,
        std::uint64_t segmentIndex);
    [[nodiscard]] core::Result<std::vector<RecoveryCandidate>> scanRecovery(
        const std::filesystem::path& packagePath, std::string_view projectName);
    [[nodiscard]] core::Result<std::vector<InterruptedSegment>> writingSegments();
    [[nodiscard]] core::Result<RecoveryResult> recover(
        const domain::SessionId& sessionId, const core::Utc& finishedAt);

private:
    SqliteProjectDatabase(internal::SqliteConnection connection, domain::ProjectId projectId);
    [[nodiscard]] core::Result<void> storeCompletedSegment(
        const domain::SessionId& sessionId, const domain::SegmentInfo& segment);
    internal::SqliteConnection connection_;
    domain::ProjectId projectId_;
};

}  // namespace creator::project_store
