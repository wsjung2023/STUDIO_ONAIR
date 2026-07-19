#pragma once

#include "core/Result.h"
#include "core/Utc.h"
#include "domain/Identifiers.h"
#include "domain/TimelineRevision.h"
#include "edit_engine/EditEngineTypes.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace creator::project_store {

struct RenderJobDiagnostics final {
    std::optional<std::string> attemptedEncoder;
    std::optional<std::string> selectedEncoder;
    std::optional<std::string> fallbackReason;
    std::optional<std::string> diagnostic;
    std::optional<std::string> outputSha256;
    std::optional<std::string> destinationIdentity;

    friend bool operator==(const RenderJobDiagnostics&,
                           const RenderJobDiagnostics&) = default;
};

struct RenderJobRecord final {
    domain::RenderJobId jobId;
    domain::ProjectId projectId;
    domain::TimelineId timelineId;
    domain::TimelineRevision timelineRevision;
    edit_engine::RenderPreset preset;
    edit_engine::RenderOverwritePolicy overwritePolicy;
    std::filesystem::path destination;
    std::filesystem::path partial;
    edit_engine::RenderProgress progress;
    RenderJobDiagnostics diagnostics;
    core::Utc createdAt;
    std::optional<core::Utc> startedAt;
    core::Utc updatedAt;
    std::optional<core::Utc> finishedAt;

    friend bool operator==(const RenderJobRecord&,
                           const RenderJobRecord&) = default;
};

struct RenderJobUpdate final {
    edit_engine::RenderProgress progress;
    RenderJobDiagnostics diagnostics;
    std::optional<core::Utc> startedAt;
    core::Utc updatedAt;
    std::optional<core::Utc> finishedAt;

    friend bool operator==(const RenderJobUpdate&,
                           const RenderJobUpdate&) = default;
};

class IRenderJobStore {
public:
    [[nodiscard]] virtual core::Result<void> begin(
        const RenderJobRecord& pending) = 0;
    [[nodiscard]] virtual core::Result<void> advance(
        const domain::RenderJobId& jobId, const RenderJobUpdate& update) = 0;
    [[nodiscard]] virtual core::Result<std::optional<RenderJobRecord>> load(
        const domain::RenderJobId& jobId) = 0;
    [[nodiscard]] virtual core::Result<std::vector<RenderJobRecord>>
    listRecoverable() = 0;
    virtual ~IRenderJobStore() = default;
};

}  // namespace creator::project_store
