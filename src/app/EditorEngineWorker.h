#pragma once

#include "core/Timebase.h"
#include "edit_engine/EditEngineTypes.h"
#include "edit_engine/IEditEngine.h"

#include <QObject>
#include <QImage>
#include <QString>

#include <memory>

namespace creator::app {

enum class EditorEngineOperation {
    Load,
    Update,
    Play,
    Pause,
    Seek,
    Frame,
};

class EditorEngineWorker final : public QObject {
    Q_OBJECT
public:
    explicit EditorEngineWorker(
        std::unique_ptr<edit_engine::IEditEngine> engine);

    void load(quint64 generation, quint64 commandId,
              edit_engine::TimelineSnapshot snapshot);
    void update(quint64 generation, quint64 commandId,
                edit_engine::TimelineChangeSet change);
    void play(quint64 generation, quint64 commandId);
    void pause(quint64 generation, quint64 commandId);
    void seek(quint64 generation, quint64 commandId,
              core::TimestampNs position);
    void requestFrame(quint64 generation, quint64 commandId,
                      core::TimestampNs position);

signals:
    void completed(quint64 generation, quint64 commandId, int operation,
                   bool success, QString errorMessage);
    void frameCompleted(quint64 generation, quint64 commandId, bool success,
                        QString errorMessage, qlonglong revision,
                        qlonglong positionNs, QImage image);

private:
    void publish(quint64 generation, quint64 commandId,
                 EditorEngineOperation operation,
                 const core::Result<void>& result);

    std::unique_ptr<edit_engine::IEditEngine> engine_;
};

}  // namespace creator::app
