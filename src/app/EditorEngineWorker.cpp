#include "app/EditorEngineWorker.h"

#include <utility>

namespace creator::app {

EditorEngineWorker::EditorEngineWorker(
    std::unique_ptr<edit_engine::IEditEngine> engine)
    : engine_(std::move(engine)) {}

void EditorEngineWorker::load(quint64 generation, quint64 commandId,
                              edit_engine::TimelineSnapshot snapshot) {
    publish(generation, commandId, EditorEngineOperation::Load,
            engine_->load(snapshot));
}

void EditorEngineWorker::update(quint64 generation, quint64 commandId,
                                edit_engine::TimelineChangeSet change) {
    publish(generation, commandId, EditorEngineOperation::Update,
            engine_->update(change));
}

void EditorEngineWorker::play(quint64 generation, quint64 commandId) {
    publish(generation, commandId, EditorEngineOperation::Play, engine_->play());
}

void EditorEngineWorker::pause(quint64 generation, quint64 commandId) {
    publish(generation, commandId, EditorEngineOperation::Pause,
            engine_->pause());
}

void EditorEngineWorker::seek(quint64 generation, quint64 commandId,
                              core::TimestampNs position) {
    publish(generation, commandId, EditorEngineOperation::Seek,
            engine_->seek(position));
}

void EditorEngineWorker::publish(quint64 generation, quint64 commandId,
                                 EditorEngineOperation operation,
                                 const core::Result<void>& result) {
    emit completed(generation, commandId, static_cast<int>(operation),
                   result.hasValue(),
                   result.hasValue()
                       ? QString{}
                       : QString::fromStdString(result.error().message()));
}

}  // namespace creator::app
