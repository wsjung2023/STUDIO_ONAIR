#include "app/ProjectEditorBinding.h"

#include "app/EditorController.h"
#include "app/ProjectController.h"

#include <QObject>

namespace creator::app {

QMetaObject::Connection bindProjectEditor(ProjectController& projects,
                                           EditorController& editor) {
    return QObject::connect(
        &projects, &ProjectController::projectOpened, &editor,
        [&projects, &editor] { editor.openProject(projects.projectUrl()); });
}

}  // namespace creator::app
