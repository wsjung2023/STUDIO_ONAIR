#pragma once

#include <QMetaObject>

namespace creator::app {

class EditorController;
class ProjectController;
class StudioRecordingBinding;

[[nodiscard]] QMetaObject::Connection bindProjectEditor(
    ProjectController& projects, EditorController& editor);
[[nodiscard]] QMetaObject::Connection bindProjectEditor(
    ProjectController& projects, EditorController& editor,
    StudioRecordingBinding& studio);

}  // namespace creator::app
