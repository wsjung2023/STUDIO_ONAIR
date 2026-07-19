#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "core/Utc.h"
#include "domain/Identifiers.h"
#include "domain/RecordingSession.h"
#include "domain/Segment.h"
#include "project_store/ProjectPackage.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace creator::project_store {

class IProjectDatabaseIdentityLease {
public:
    [[nodiscard]] virtual core::Result<void> verifyCurrentIdentity() const = 0;
    virtual ~IProjectDatabaseIdentityLease() = default;
};

struct OpenProjectResult final {
    ProjectPackage package;
    std::vector<RecoveryCandidate> recoveryCandidates;
    std::filesystem::path databasePath;
    std::shared_ptr<const IProjectDatabaseIdentityLease> databaseIdentityLease;
};

class IProjectPackageStore {
public:
    virtual ~IProjectPackageStore() = default;
    IProjectPackageStore(const IProjectPackageStore&) = delete;
    IProjectPackageStore& operator=(const IProjectPackageStore&) = delete;
    IProjectPackageStore(IProjectPackageStore&&) = delete;
    IProjectPackageStore& operator=(IProjectPackageStore&&) = delete;

    [[nodiscard]] virtual core::Result<OpenProjectResult> create(
        const std::filesystem::path& packagePath, const std::string& name) = 0;
    [[nodiscard]] virtual core::Result<OpenProjectResult> open(
        const std::filesystem::path& packagePath) = 0;
    [[nodiscard]] virtual core::Result<void> beginRecording(
        const std::filesystem::path& packagePath, const domain::SessionId& sessionId,
        core::TimestampNs startedAt, const core::Utc& createdAt) = 0;
    [[nodiscard]] virtual core::Result<void> completeRecording(
        const std::filesystem::path& packagePath, const domain::RecordingSession& session,
        const core::Utc& finishedAt) = 0;
    [[nodiscard]] virtual core::Result<void> abortRecording(
        const std::filesystem::path& packagePath, const domain::SessionId& sessionId,
        std::string_view reason, const core::Utc& finishedAt) = 0;
    [[nodiscard]] virtual core::Result<void> beginSegment(
        const std::filesystem::path& packagePath, const domain::SessionId& sessionId,
        const domain::SegmentInfo& segment) = 0;
    [[nodiscard]] virtual core::Result<void> markSegmentReady(
        const std::filesystem::path& packagePath, const domain::SessionId& sessionId,
        const domain::SegmentInfo& segment) = 0;
    [[nodiscard]] virtual core::Result<void> markSegmentFailed(
        const std::filesystem::path& packagePath, const domain::SessionId& sessionId,
        const domain::SourceId& sourceId, std::uint64_t segmentIndex) = 0;
    [[nodiscard]] virtual core::Result<RecoveryResult> recover(
        const std::filesystem::path& packagePath, const domain::SessionId& sessionId,
        const core::Utc& finishedAt) = 0;

protected:
    IProjectPackageStore() = default;
};

}  // namespace creator::project_store
