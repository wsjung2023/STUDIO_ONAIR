#pragma once

#include "app/RecordingTimelineReconciler.h"
#include "core/Result.h"
#include "core/Timebase.h"
#include "domain/StudioScene.h"
#include "project_store/IStudioStore.h"

#include <QMetaType>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace creator::app {

using StudioStoreOpenFactory = std::function<core::Result<
    std::unique_ptr<project_store::IStudioStore>>(
    const std::filesystem::path& packageRoot)>;
using StudioIdentityFactory = std::function<std::string()>;

enum class StudioWorkflowOperation {
    AddScene,
    DuplicateScene,
    RenameScene,
    RemoveScene,
    MoveScene,
    SwitchScene,
    ToggleSource,
    MoveSource,
    SetSourceTransform,
    SetSourcePipPreset,
    ResetSourceTransform,
    PrepareRecording,
    AbortRecording,
    CompleteRecording,
    AddMarker,
    RetryReconciliation,
};

struct StudioWorkflowRequest final {
    StudioWorkflowOperation operation;
    std::string sceneId;
    std::string sourceId;
    std::string text;
    int direction{0};
    std::optional<domain::VisualTransform> transform;
    std::optional<domain::SessionId> sessionId;
    std::vector<project_store::RecordingSourceRole> recordingSources;
    core::TimestampNs position{};
};

struct StudioWorkflowState final {
    project_store::StudioSnapshot snapshot;
    std::optional<domain::SceneId> selectedSceneId;
    std::optional<domain::SourceId> selectedSourceId;
    bool recording{false};
    bool reconciling{false};
    std::optional<domain::SessionId> activeSessionId;
    std::optional<domain::SessionId> reconciliationSessionId;
    core::TimestampNs recordingPosition{};
    std::uint64_t nextSceneEventSequence{1};
    std::size_t markerCount{0};
    std::string status;

    friend bool operator==(const StudioWorkflowState&,
                           const StudioWorkflowState&) = default;
};

using StudioWorkflowResult = core::Result<StudioWorkflowState>;
using StudioWorkflowResultPtr =
    std::shared_ptr<const StudioWorkflowResult>;

}  // namespace creator::app

Q_DECLARE_METATYPE(creator::app::StudioWorkflowResultPtr)
