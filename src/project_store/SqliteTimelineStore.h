#pragma once

#include "core/Result.h"
#include "domain/Identifiers.h"
#include "project_store/ITimelineStore.h"
#include "project_store/internal/SqliteConnection.h"

#include <filesystem>
#include <utility>

namespace creator::project_store {

class SqliteTimelineStore final : public ITimelineStore {
public:
    [[nodiscard]] static core::Result<SqliteTimelineStore> open(
        const std::filesystem::path& databasePath,
        const domain::ProjectId& expectedProjectId);

    SqliteTimelineStore(SqliteTimelineStore&&) noexcept = default;
    SqliteTimelineStore& operator=(SqliteTimelineStore&&) noexcept = default;
    SqliteTimelineStore(const SqliteTimelineStore&) = delete;
    SqliteTimelineStore& operator=(const SqliteTimelineStore&) = delete;

    [[nodiscard]] core::Result<void> putAsset(
        const domain::MediaAsset& asset) override;
    [[nodiscard]] core::Result<domain::MediaAsset> asset(
        const domain::AssetId& assetId) override;
    [[nodiscard]] core::Result<std::vector<domain::MediaAsset>> assets() override;
    [[nodiscard]] core::Result<void> createTimeline(
        const domain::Timeline& timeline) override;
    [[nodiscard]] core::Result<PersistedTimeline> loadPrimaryTimeline() override;
    [[nodiscard]] core::Result<void> commitEdit(
        const TimelineCommit& commit) override;

private:
    SqliteTimelineStore(internal::SqliteConnection connection,
                        domain::ProjectId projectId)
        : connection_(std::move(connection)), projectId_(std::move(projectId)) {}

    [[nodiscard]] core::Result<void> writeSnapshot(
        const domain::Timeline& timeline);

    internal::SqliteConnection connection_;
    domain::ProjectId projectId_;
};

}  // namespace creator::project_store
