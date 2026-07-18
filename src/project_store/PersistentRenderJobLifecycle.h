#pragma once

#include "edit_engine/IRenderJobLifecycle.h"
#include "project_store/IRenderJobStore.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace creator::project_store {

class PersistentRenderJobLifecycle final
    : public edit_engine::IRenderJobLifecycle {
public:
    explicit PersistentRenderJobLifecycle(
        std::shared_ptr<IRenderJobStore> store);

    [[nodiscard]] core::Result<void> begin(
        const edit_engine::RenderRequest& request,
        const std::filesystem::path& partial,
        core::DurationNs totalDuration) override;
    [[nodiscard]] core::Result<void> encoderSelected(
        const domain::RenderJobId& jobId,
        const edit_engine::RenderEncoderDiagnostics& diagnostics) override;
    [[nodiscard]] core::Result<void> advance(
        const domain::RenderJobId& jobId,
        const edit_engine::RenderProgress& progress) override;
    [[nodiscard]] core::Result<void> preparePublication(
        const domain::RenderJobId& jobId,
        const std::filesystem::path& partial,
        const edit_engine::RenderProgress& progress) override;
    [[nodiscard]] core::Result<void> finish(
        const domain::RenderJobId& jobId,
        edit_engine::RenderJobState state,
        std::string diagnostic) override;

private:
    struct Session final {
        edit_engine::RenderProgress persistedProgress;
        RenderJobDiagnostics diagnostics;
        core::Utc startedAt;
    };

    [[nodiscard]] core::Result<void> persist(
        const domain::RenderJobId& jobId, Session& session,
        const edit_engine::RenderProgress& progress,
        bool terminal);

    std::shared_ptr<IRenderJobStore> store_;
    std::mutex mutex_;
    std::unordered_map<std::string, Session> sessions_;
};

}  // namespace creator::project_store
