#pragma once

#include "project_store/IStudioStore.h"
#include "project_store/internal/SqliteConnection.h"

#include <filesystem>
#include <string_view>
#include <utility>

namespace creator::project_store {

class SqliteStudioStore final : public IStudioStore {
public:
    [[nodiscard]] static core::Result<SqliteStudioStore> open(
        const std::filesystem::path& databasePath,
        const domain::ProjectId& expectedProjectId);

    SqliteStudioStore(SqliteStudioStore&&) noexcept = default;
    SqliteStudioStore& operator=(SqliteStudioStore&&) noexcept = default;
    SqliteStudioStore(const SqliteStudioStore&) = delete;
    SqliteStudioStore& operator=(const SqliteStudioStore&) = delete;

    [[nodiscard]] core::Result<void> seedDefaultsIfEmpty(
        const std::vector<domain::StudioScene>& scenes) override;
    [[nodiscard]] core::Result<StudioSnapshot> load() override;
    [[nodiscard]] core::Result<void> commitSceneMutation(
        const StudioSnapshot& snapshot) override;
    [[nodiscard]] core::Result<void> prepareRecording(
        const domain::SessionId& sessionId,
        const std::vector<RecordingSourceRole>& sources,
        const domain::SceneId& activeSceneId) override;
    [[nodiscard]] core::Result<void> discardRecordingPreparation(
        const domain::SessionId& sessionId) override;
    [[nodiscard]] core::Result<void> recordSceneSwitch(
        const domain::SessionId& sessionId,
        const domain::SceneId& sceneId, std::uint64_t sequence,
        core::TimestampNs position) override;
    [[nodiscard]] core::Result<void> recordMarker(
        const RecordingMarker& marker) override;
    [[nodiscard]] core::Result<std::vector<RecordingSourceRole>>
        loadRecordingSources(const domain::SessionId& sessionId) override;
    [[nodiscard]] core::Result<std::vector<RecordingSceneEvent>>
        loadRecordingSceneEvents(const domain::SessionId& sessionId) override;
    [[nodiscard]] core::Result<std::vector<RecordingMarker>>
        loadRecordingMarkers(const domain::SessionId& sessionId) override;
    [[nodiscard]] core::Result<std::vector<UnimportedRecording>>
        completedUnimportedRecordings() override;
    [[nodiscard]] core::Result<void> putRecordingImport(
        const RecordingImportRecord& record) override;
    [[nodiscard]] core::Result<std::optional<RecordingImportRecord>>
        recordingImport(const domain::SessionId& sessionId) override;

private:
    SqliteStudioStore(internal::SqliteConnection connection,
                      domain::ProjectId projectId)
        : connection_(std::move(connection)), projectId_(std::move(projectId)) {}

    [[nodiscard]] core::Result<void> writeScene(
        const domain::StudioScene& scene, std::string_view createdAtUtc);
    [[nodiscard]] core::Result<void> writeSceneSources(
        const domain::StudioScene& scene);
    [[nodiscard]] core::Result<void> ensureSessionBelongsToProject(
        const domain::SessionId& sessionId);
    [[nodiscard]] core::Result<void> ensureSessionIsRecording(
        const domain::SessionId& sessionId);
    [[nodiscard]] core::Result<void> ensureSessionIsImportable(
        const domain::SessionId& sessionId);

    internal::SqliteConnection connection_;
    domain::ProjectId projectId_;
};

}  // namespace creator::project_store
