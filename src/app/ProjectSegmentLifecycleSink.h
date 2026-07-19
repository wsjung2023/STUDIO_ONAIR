#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "core/Utc.h"
#include "domain/RecordingSession.h"
#include "project_store/IProjectPackageStore.h"
#include "recorder/TrackSegmentPorts.h"

#include <filesystem>
#include <memory>
#include <mutex>

namespace creator::app {

class ProjectSegmentLifecycleContext final {
public:
    [[nodiscard]] static core::Result<std::shared_ptr<ProjectSegmentLifecycleContext>> create(
        std::shared_ptr<project_store::IProjectPackageStore> store,
        std::filesystem::path packagePath, domain::RecordingSession session);

    [[nodiscard]] core::Result<void> begin(const domain::SegmentInfo& segment);
    [[nodiscard]] core::Result<void> ready(const domain::SegmentInfo& segment);
    [[nodiscard]] core::Result<void> failed(const domain::SourceId& sourceId,
                                            std::uint64_t segmentIndex);
    [[nodiscard]] domain::RecordingSession sessionSnapshot() const;
    [[nodiscard]] core::Result<domain::RecordingSession> complete(
        core::TimestampNs stoppedAt, const core::Utc& finishedAt);

private:
    ProjectSegmentLifecycleContext(
        std::shared_ptr<project_store::IProjectPackageStore> store,
        std::filesystem::path packagePath, domain::RecordingSession session);

    std::shared_ptr<project_store::IProjectPackageStore> store_;
    std::filesystem::path packagePath_;
    mutable std::mutex mutex_;
    domain::RecordingSession session_;
};

class ProjectSegmentLifecycleSink final : public recorder::ISegmentLifecycleSink {
public:
    explicit ProjectSegmentLifecycleSink(
        std::shared_ptr<ProjectSegmentLifecycleContext> context);

    [[nodiscard]] core::Result<void> begin(
        const domain::SegmentInfo& segment) override;
    [[nodiscard]] core::Result<void> ready(
        const domain::SegmentInfo& segment) override;
    [[nodiscard]] core::Result<void> failed(
        const domain::SourceId& sourceId, std::uint64_t segmentIndex) override;

private:
    std::shared_ptr<ProjectSegmentLifecycleContext> context_;
};

}  // namespace creator::app
