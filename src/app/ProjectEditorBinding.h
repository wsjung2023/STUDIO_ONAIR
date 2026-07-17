#pragma once

#include <QMetaObject>

namespace creator::app {

class EditorController;
class ProjectController;

[[nodiscard]] QMetaObject::Connection bindProjectEditor(
    ProjectController& projects, EditorController& editor);

}  // namespace creator::app
