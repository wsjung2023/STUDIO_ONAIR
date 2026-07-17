#pragma once

#include "app/EditorSessionTypes.h"
#include "app/TimelineEditService.h"
#include "project_store/IProjectPackageStore.h"
#include "project_store/ITimelineStore.h"

#include <QMetaType>
#include <QObject>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace creator::app {

using TimelineStoreFactory = std::function<core::Result<
    std::unique_ptr<project_store::ITimelineStore>>(
    const std::filesystem::path&, const domain::ProjectId&)>;
using GeneratedOverlayFactory = std::function<core::Result<
    std::vector<edit_engine::GeneratedOverlayDescriptor>>(
    const edit_engine::TimelineSnapshot&)>;

class EditorSessionWorker final : public QObject {
    Q_OBJECT
public:
    EditorSessionWorker();
    EditorSessionWorker(
        std::unique_ptr<project_store::IProjectPackageStore> packageStore,
        TimelineStoreFactory timelineStoreFactory,
        GeneratedOverlayFactory generatedOverlayFactory = {});

    void openProject(quint64 generation, std::filesystem::path packageRoot);
    void edit(quint64 generation, quint64 commandId, EditorEditRequest request);

signals:
    void opened(quint64 generation, creator::app::EditorSessionResultPtr result);
    void edited(quint64 generation, quint64 commandId,
                creator::app::EditorSessionResultPtr result);

private:
    void publishError(quint64 generation, core::AppError error);
    void publishEditError(quint64 generation, quint64 commandId,
                          core::AppError error);
    [[nodiscard]] core::Result<EditorSessionState> currentState(
        std::optional<core::AppError>* derivedWorkDiagnostic) const;

    std::unique_ptr<project_store::IProjectPackageStore> packageStore_;
    TimelineStoreFactory timelineStoreFactory_;
    std::unique_ptr<project_store::ITimelineStore> timelineStore_;
    std::optional<TimelineEditService> editService_;
    std::filesystem::path packageRoot_;
    std::vector<domain::MediaAsset> assets_;
    domain::CanvasSettings canvas_{};
    GeneratedOverlayFactory generatedOverlayFactory_;
};

}  // namespace creator::app

Q_DECLARE_METATYPE(creator::app::EditorSessionResultPtr)
