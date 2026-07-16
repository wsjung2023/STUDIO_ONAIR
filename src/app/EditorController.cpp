#include "app/EditorController.h"

#include "app/EditorEngineWorker.h"

#include <QMetaObject>

#include <utility>

namespace creator::app {

EditorController::EditorController(
    std::unique_ptr<edit_engine::IEditEngine> engine, QObject* parent)
    : QObject(parent), worker_(new EditorEngineWorker{std::move(engine)}) {
    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &EditorEngineWorker::completed, this,
            &EditorController::handleCompleted);
    workerThread_.start();
}

EditorController::~EditorController() {
    while (!queuedCommands_.empty()) {
        auto command = std::move(queuedCommands_.front());
        queuedCommands_.pop_front();
        postToWorker(std::move(command));
    }
    QMetaObject::invokeMethod(worker_, [] {}, Qt::BlockingQueuedConnection);
    disconnect(worker_, nullptr, this, nullptr);
    workerThread_.quit();
    workerThread_.wait();
}

qlonglong EditorController::timelineRevision() const noexcept {
    return snapshot_.has_value() ? snapshot_->revision.value() : -1;
}

void EditorController::openSession(
    std::vector<domain::MediaAsset> assets,
    edit_engine::TimelineSnapshot snapshot) {
    ++generation_;
    mediaBinModel_.setAssets(std::move(assets));
    timelineTrackModel_.setTimeline(snapshot.timeline);
    snapshot_ = snapshot;
    setPlaying(false);
    if (playhead_ != core::TimestampNs{}) {
        playhead_ = core::TimestampNs{};
        emit playheadChanged();
    }
    setStatus({});
    setPreviewStale(true);
    emit timelineChanged();
    queueLoad(std::move(snapshot));
}

void EditorController::commitTimeline(edit_engine::TimelineChangeSet change) {
    if (!snapshot_.has_value()) {
        setStatus(QStringLiteral("No editor session is open"));
        return;
    }
    if (change.baseRevision() != snapshot_->revision ||
        change.target().timeline.id() != snapshot_->timeline.id()) {
        setStatus(QStringLiteral("Timeline update is stale or changes session identity"));
        return;
    }
    timelineTrackModel_.setTimeline(change.target().timeline);
    snapshot_ = change.target();
    emit timelineChanged();
    queueUpdate(std::move(change));
}

void EditorController::play() {
    if (!snapshot_.has_value()) {
        setStatus(QStringLiteral("No editor session is open"));
        return;
    }
    queueSimple(EditorEngineOperation::Play);
}

void EditorController::pause() {
    if (!snapshot_.has_value()) {
        setStatus(QStringLiteral("No editor session is open"));
        return;
    }
    queueSimple(EditorEngineOperation::Pause);
}

void EditorController::seek(qlonglong positionNs) {
    if (!snapshot_.has_value()) {
        setStatus(QStringLiteral("No editor session is open"));
        return;
    }
    if (positionNs < 0) {
        setStatus(QStringLiteral("Editor position must not be negative"));
        return;
    }
    queueSimple(EditorEngineOperation::Seek,
                core::TimestampNs{core::DurationNs{positionNs}});
}

void EditorController::queueLoad(edit_engine::TimelineSnapshot snapshot,
                                 bool recoveryPriority) {
    const quint64 commandId = beginCommand(EditorEngineOperation::Load, std::nullopt);
    QueuedCommand command{generation_, commandId, EditorEngineOperation::Load,
                          std::nullopt, std::move(snapshot), std::nullopt};
    if (recoveryPriority) {
        queuedCommands_.push_front(std::move(command));
    } else {
        queuedCommands_.push_back(std::move(command));
    }
    dispatchNext();
}

void EditorController::queueUpdate(edit_engine::TimelineChangeSet change) {
    const quint64 commandId =
        beginCommand(EditorEngineOperation::Update, std::nullopt);
    queuedCommands_.push_back(
        QueuedCommand{generation_, commandId, EditorEngineOperation::Update,
                      std::nullopt, std::nullopt, std::move(change)});
    dispatchNext();
}

void EditorController::queueSimple(
    EditorEngineOperation operation,
    std::optional<core::TimestampNs> position) {
    const quint64 commandId = beginCommand(operation, position);
    queuedCommands_.push_back(QueuedCommand{generation_, commandId, operation,
                                             position, std::nullopt,
                                             std::nullopt});
    dispatchNext();
}

void EditorController::dispatchNext() {
    if (workerCommandActive_ || queuedCommands_.empty()) return;
    workerCommandActive_ = true;
    auto command = std::move(queuedCommands_.front());
    queuedCommands_.pop_front();
    postToWorker(std::move(command));
}

void EditorController::postToWorker(QueuedCommand command) {
    const quint64 generation = command.generation;
    const quint64 commandId = command.commandId;
    switch (command.operation) {
        case EditorEngineOperation::Play:
            QMetaObject::invokeMethod(
                worker_,
                [worker = worker_, generation, commandId] {
                    worker->play(generation, commandId);
                },
                Qt::QueuedConnection);
            break;
        case EditorEngineOperation::Pause:
            QMetaObject::invokeMethod(
                worker_,
                [worker = worker_, generation, commandId] {
                    worker->pause(generation, commandId);
                },
                Qt::QueuedConnection);
            break;
        case EditorEngineOperation::Seek: {
            const core::TimestampNs position = *command.position;
            QMetaObject::invokeMethod(
                worker_,
                [worker = worker_, generation, commandId, position] {
                    worker->seek(generation, commandId, position);
                },
                Qt::QueuedConnection);
            break;
        }
        case EditorEngineOperation::Load: {
            auto snapshot = std::move(*command.snapshot);
            QMetaObject::invokeMethod(
                worker_,
                [worker = worker_, generation, commandId,
                 snapshot = std::move(snapshot)]() mutable {
                    worker->load(generation, commandId, std::move(snapshot));
                },
                Qt::QueuedConnection);
            break;
        }
        case EditorEngineOperation::Update: {
            auto change = std::move(*command.change);
            QMetaObject::invokeMethod(
                worker_,
                [worker = worker_, generation, commandId,
                 change = std::move(change)]() mutable {
                    worker->update(generation, commandId, std::move(change));
                },
                Qt::QueuedConnection);
            break;
        }
    }
}

quint64 EditorController::beginCommand(
    EditorEngineOperation operation,
    std::optional<core::TimestampNs> position) {
    const bool wasBusy = busy();
    const quint64 commandId = nextCommandId_++;
    commands_.emplace(commandId,
                      PendingCommand{generation_, operation, position});
    ++pendingCommands_;
    if (!wasBusy) emit busyChanged();
    return commandId;
}

void EditorController::handleCompleted(quint64 generation, quint64 commandId,
                                       int operationValue, bool success,
                                       const QString& errorMessage) {
    const auto found = commands_.find(commandId);
    if (found == commands_.end()) return;
    const PendingCommand command = found->second;
    const auto operation = static_cast<EditorEngineOperation>(operationValue);
    const bool current = generation == generation_ &&
                         command.generation == generation_ &&
                         command.operation == operation;
    commands_.erase(found);
    --pendingCommands_;
    workerCommandActive_ = false;

    if (current) {
        if (!success) {
            setStatus(errorMessage);
            if (operation == EditorEngineOperation::Load ||
                operation == EditorEngineOperation::Update) {
                setPreviewStale(true);
            }
            if (operation == EditorEngineOperation::Update &&
                snapshot_.has_value()) {
                queueLoad(*snapshot_, true);
            }
        } else {
            switch (operation) {
                case EditorEngineOperation::Load:
                    setPreviewStale(false);
                    break;
                case EditorEngineOperation::Play:
                    setPlaying(true);
                    break;
                case EditorEngineOperation::Pause:
                    setPlaying(false);
                    break;
                case EditorEngineOperation::Seek:
                    playhead_ = *command.position;
                    emit playheadChanged();
                    break;
                case EditorEngineOperation::Update:
                    break;
            }
        }
    }
    if (!busy()) emit busyChanged();
    dispatchNext();
}

void EditorController::setPreviewStale(bool value) {
    if (previewStale_ == value) return;
    previewStale_ = value;
    emit previewStaleChanged();
}

void EditorController::setPlaying(bool value) {
    if (playing_ == value) return;
    playing_ = value;
    emit playingChanged();
}

void EditorController::setStatus(QString value) {
    if (statusMessage_ == value) return;
    statusMessage_ = std::move(value);
    emit statusMessageChanged();
}

}  // namespace creator::app
