#pragma once

#include "project_store/IProjectPackageStore.h"

namespace creator::project_store {

class ProjectPackageStore final : public IProjectPackageStore {
public:
    [[nodiscard]] core::Result<OpenProjectResult> create(
        const std::filesystem::path& packagePath, const std::string& name) override;
    [[nodiscard]] core::Result<OpenProjectResult> open(
        const std::filesystem::path& packagePath) override;
    [[nodiscard]] core::Result<void> beginRecording(
        const std::filesystem::path& packagePath, const domain::SessionId& sessionId,
        core::TimestampNs startedAt, const core::Utc& createdAt) override;
    [[nodiscard]] core::Result<void> completeRecording(
        const std::filesystem::path& packagePath, const domain::RecordingSession& session,
        const core::Utc& finishedAt) override;
    [[nodiscard]] core::Result<void> abortRecording(
        const std::filesystem::path& packagePath, const domain::SessionId& sessionId,
        std::string_view reason, const core::Utc& finishedAt) override;
    [[nodiscard]] core::Result<void> beginSegment(
        const std::filesystem::path& packagePath, const domain::SessionId& sessionId,
        const domain::SegmentInfo& segment) override;
    [[nodiscard]] core::Result<void> markSegmentReady(
        const std::filesystem::path& packagePath, const domain::SessionId& sessionId,
        const domain::SegmentInfo& segment) override;
    [[nodiscard]] core::Result<void> markSegmentFailed(
        const std::filesystem::path& packagePath, const domain::SessionId& sessionId,
        const domain::SourceId& sourceId, std::uint64_t segmentIndex) override;
    [[nodiscard]] core::Result<RecoveryResult> recover(
        const std::filesystem::path& packagePath, const domain::SessionId& sessionId,
        const core::Utc& finishedAt) override;
};

}  // namespace creator::project_store
