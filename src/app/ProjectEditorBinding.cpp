#include "app/ProjectEditorBinding.h"

#include "app/EditorController.h"
#include "app/ProjectController.h"
#include "app/StudioRecordingBinding.h"

#include <QObject>

namespace creator::app {

QMetaObject::Connection bindProjectEditor(ProjectController& projects,
                                           EditorController& editor) {
    return QObject::connect(
        &projects, &ProjectController::projectOpened, &editor,
        [&projects, &editor] { editor.openProject(projects.projectUrl()); });
}

QMetaObject::Connection bindProjectEditor(ProjectController& projects,
                                           EditorController& editor,
                                           StudioRecordingBinding& studio) {
    QObject::connect(&projects, &ProjectController::projectOpened, &studio,
                     [&projects, &studio] {
                         studio.openProject(projects.projectUrl());
                     });
    return QObject::connect(
        &studio, &StudioRecordingBinding::timelineReconciled, &editor,
        [&editor](const QUrl& projectUrl) { editor.openProject(projectUrl); });
}

}  // namespace creator::app
