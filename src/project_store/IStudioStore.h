#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "core/Utc.h"
#include "domain/Identifiers.h"
#include "domain/StudioScene.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace creator::project_store {

struct StudioSnapshot final {
    std::vector<domain::StudioScene> scenes;
    domain::SceneId activeSceneId;

    friend bool operator==(const StudioSnapshot&, const StudioSnapshot&) = default;
};

struct RecordingSourceRole final {
    domain::SourceId sourceId;
    domain::StudioSourceRole role;

    friend bool operator==(const RecordingSourceRole&,
                           const RecordingSourceRole&) = default;
};

struct RecordingSceneEvent final {
    domain::SessionId sessionId;
    std::uint64_t sequence;
    domain::SceneId sceneId;
    core::TimestampNs position;

    friend bool operator==(const RecordingSceneEvent&,
                           const RecordingSceneEvent&) = default;
};

struct RecordingMarker final {
    std::string markerId;
    domain::SessionId sessionId;
    core::TimestampNs position;
    std::string label;

    friend bool operator==(const RecordingMarker&, const RecordingMarker&) = default;
};

struct RecordingImportRecord final {
    domain::SessionId sessionId;
    domain::TimelineId timelineId;
    core::TimestampNs base;
    std::int64_t importedRevision;
    core::Utc importedAt;

    friend bool operator==(const RecordingImportRecord&,
                           const RecordingImportRecord&) = default;
};

struct UnimportedRecording final {
    domain::SessionId sessionId;
    core::TimestampNs startedAt;
    core::TimestampNs stoppedAt;

    friend bool operator==(const UnimportedRecording&,
                           const UnimportedRecording&) = default;
};

class IStudioStore {
public:
    virtual ~IStudioStore() = default;

    [[nodiscard]] virtual core::Result<void> seedDefaultsIfEmpty(
        const std::vector<domain::StudioScene>& scenes) = 0;
    [[nodiscard]] virtual core::Result<StudioSnapshot> load() = 0;
    [[nodiscard]] virtual core::Result<void> commitSceneMutation(
        const StudioSnapshot& snapshot) = 0;
    [[nodiscard]] virtual core::Result<void> prepareRecording(
        const domain::SessionId& sessionId,
        const std::vector<RecordingSourceRole>& sources,
        const domain::SceneId& activeSceneId) = 0;
    [[nodiscard]] virtual core::Result<void> discardRecordingPreparation(
        const domain::SessionId& sessionId) = 0;
    [[nodiscard]] virtual core::Result<void> recordSceneSwitch(
        const domain::SessionId& sessionId,
        const domain::SceneId& sceneId, std::uint64_t sequence,
        core::TimestampNs position) = 0;
    [[nodiscard]] virtual core::Result<void> recordMarker(
        const RecordingMarker& marker) = 0;
    [[nodiscard]] virtual core::Result<std::vector<RecordingSourceRole>>
        loadRecordingSources(const domain::SessionId& sessionId) = 0;
    [[nodiscard]] virtual core::Result<std::vector<RecordingSceneEvent>>
        loadRecordingSceneEvents(const domain::SessionId& sessionId) = 0;
    [[nodiscard]] virtual core::Result<std::vector<RecordingMarker>>
        loadRecordingMarkers(const domain::SessionId& sessionId) = 0;
    [[nodiscard]] virtual core::Result<std::vector<UnimportedRecording>>
        completedUnimportedRecordings() = 0;
    [[nodiscard]] virtual core::Result<void> putRecordingImport(
        const RecordingImportRecord& record) = 0;
    [[nodiscard]] virtual core::Result<std::optional<RecordingImportRecord>>
        recordingImport(const domain::SessionId& sessionId) = 0;
};

}  // namespace creator::project_store
