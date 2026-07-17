#pragma once

#include "app/StudioWorkflowTypes.h"

#include <QObject>

#include <filesystem>
#include <memory>
#include <optional>

namespace creator::app {

class StudioWorkflowWorker final : public QObject {
    Q_OBJECT
public:
    StudioWorkflowWorker(
        StudioStoreOpenFactory storeFactory,
        std::unique_ptr<IRecordingTimelineReconciler> reconciler,
        StudioIdentityFactory identityFactory);

    void openProject(quint64 generation, std::filesystem::path packageRoot);
    void execute(quint64 generation, quint64 commandId,
                 StudioWorkflowRequest request);

signals:
    void completed(quint64 generation, quint64 commandId,
                   creator::app::StudioWorkflowResultPtr result);
    void reconciliationProgress(quint64 generation, bool active);

private:
    [[nodiscard]] core::Result<StudioWorkflowState> apply(
        const StudioWorkflowRequest& request);
    [[nodiscard]] core::Result<void> publishSceneSnapshot(
        project_store::StudioSnapshot snapshot, StudioWorkflowState& staged);
    [[nodiscard]] core::Result<void> reconcileSession(
        const domain::SessionId& sessionId, StudioWorkflowState& staged);
    void emitResult(quint64 generation, quint64 commandId,
                    StudioWorkflowResult result);

    StudioStoreOpenFactory storeFactory_;
    std::unique_ptr<IRecordingTimelineReconciler> reconciler_;
    StudioIdentityFactory identityFactory_;
    std::unique_ptr<project_store::IStudioStore> store_;
    std::filesystem::path packageRoot_;
    std::optional<StudioWorkflowState> state_;
    quint64 activeGeneration_{0};
};

}  // namespace creator::app
