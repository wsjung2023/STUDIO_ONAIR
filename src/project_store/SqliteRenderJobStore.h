#pragma once

#include "project_store/IRenderJobStore.h"
#include "project_store/internal/SqliteConnection.h"

#include <filesystem>

namespace creator::project_store {

class SqliteRenderJobStore final : public IRenderJobStore {
public:
    [[nodiscard]] static core::Result<SqliteRenderJobStore> open(
        const std::filesystem::path& databasePath,
        const domain::ProjectId& expectedProjectId,
        internal::SqliteConnection::IdentityVerifier identityVerifier = {});

    SqliteRenderJobStore(SqliteRenderJobStore&&) noexcept = default;
    SqliteRenderJobStore& operator=(SqliteRenderJobStore&&) noexcept = default;
    SqliteRenderJobStore(const SqliteRenderJobStore&) = delete;
    SqliteRenderJobStore& operator=(const SqliteRenderJobStore&) = delete;

    [[nodiscard]] core::Result<void> begin(
        const RenderJobRecord& pending) override;
    [[nodiscard]] core::Result<void> advance(
        const domain::RenderJobId& jobId,
        const RenderJobUpdate& update) override;
    [[nodiscard]] core::Result<std::optional<RenderJobRecord>> load(
        const domain::RenderJobId& jobId) override;
    [[nodiscard]] core::Result<std::vector<RenderJobRecord>>
    listRecoverable() override;

private:
    SqliteRenderJobStore(internal::SqliteConnection connection,
                         domain::ProjectId projectId)
        : connection_(std::move(connection)),
          projectId_(std::move(projectId)) {}

    internal::SqliteConnection connection_;
    domain::ProjectId projectId_;
};

}  // namespace creator::project_store
