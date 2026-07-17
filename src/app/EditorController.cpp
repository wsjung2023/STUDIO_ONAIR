#include "app/EditorController.h"

#include "app/EditorEngineWorker.h"
#include "app/EditorSessionWorker.h"
#include "app/RecentProjectRegistry.h"

#include <QMetaObject>

#include <algorithm>
#include <utility>

namespace creator::app {
namespace {

std::int64_t timelineFrameCount(const domain::Timeline& timeline) {
    core::TimestampNs end{};
    for (const auto& track : timeline.tracks()) {
        for (const auto& clip : track.clips()) {
            end = std::max(end, clip.timelineRange().end());
        }
    }
    return core::timestampToFrame(end, timeline.frameRate());
}

}  // namespace

EditorController::EditorController(
    std::unique_ptr<edit_engine::IEditEngine> engine, QObject* parent)
    : QObject(parent),
      worker_(new EditorEngineWorker{std::move(engine)}),
      sessionWorker_(new EditorSessionWorker) {
    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &EditorEngineWorker::completed, this,
            &EditorController::handleCompleted);
    connect(worker_, &EditorEngineWorker::frameCompleted, this,
            &EditorController::handleFrameCompleted);
    sessionWorker_->moveToThread(&sessionThread_);
    connect(&sessionThread_, &QThread::finished, sessionWorker_,
            &QObject::deleteLater);
    connect(sessionWorker_, &EditorSessionWorker::opened, this,
            &EditorController::handleSessionOpened);
    connect(sessionWorker_, &EditorSessionWorker::edited, this,
            &EditorController::handleSessionEdited);
    playbackTimer_.setTimerType(Qt::PreciseTimer);
    connect(&playbackTimer_, &QTimer::timeout, this,
            &EditorController::requestPlaybackFrame);
    workerThread_.start();
    sessionThread_.start();
}

EditorController::~EditorController() {
    playbackTimer_.stop();
    QMetaObject::invokeMethod(sessionWorker_, [] {},
                              Qt::BlockingQueuedConnection);
    disconnect(sessionWorker_, nullptr, this, nullptr);
    sessionThread_.quit();
    sessionThread_.wait();
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

qlonglong EditorController::timelineDurationNs() const noexcept {
    if (!snapshot_.has_value()) return 0;
    core::TimestampNs end{};
    for (const auto& track : snapshot_->timeline.tracks()) {
        for (const auto& clip : track.clips()) {
            end = std::max(end, clip.timelineRange().end());
        }
    }
    return end.time_since_epoch().count();
}

const domain::Clip* EditorController::selectedClip() const noexcept {
    if (!snapshot_.has_value() || selectedTrackId_.isEmpty() ||
        selectedClipId_.isEmpty()) {
        return nullptr;
    }
    auto trackId = domain::TrackId::create(
        selectedTrackId_.toUtf8().toStdString());
    auto clipId = domain::ClipId::create(
        selectedClipId_.toUtf8().toStdString());
    if (!trackId.hasValue() || !clipId.hasValue()) return nullptr;
    return snapshot_->timeline.clip(trackId.value(), clipId.value());
}

qlonglong EditorController::selectedClipStartNs() const noexcept {
    const auto* clip = selectedClip();
    return clip == nullptr
               ? -1
               : clip->timelineRange().start().time_since_epoch().count();
}

qlonglong EditorController::selectedClipEndNs() const noexcept {
    const auto* clip = selectedClip();
    return clip == nullptr
               ? -1
               : clip->timelineRange().end().time_since_epoch().count();
}

void EditorController::openSession(
    std::vector<domain::MediaAsset> assets,
    edit_engine::TimelineSnapshot snapshot) {
    ++generation_;
    snapshot.assets = assets;
    mediaBinModel_.setAssets(std::move(assets));
    timelineTrackModel_.setTimeline(snapshot.timeline);
    snapshot_ = snapshot;
    if (!selectedTrackId_.isEmpty() || !selectedClipId_.isEmpty()) {
        selectedTrackId_.clear();
        selectedClipId_.clear();
        emit selectionChanged();
    }
    if (rangeIn_.has_value() || rangeOut_.has_value()) {
        rangeIn_.reset();
        rangeOut_.reset();
        emit markedRangeChanged();
    }
    if (canUndo_ || canRedo_ || !clean_) {
        canUndo_ = false;
        canRedo_ = false;
        clean_ = true;
        emit editStateChanged();
    }
    setPlaying(false);
    if (playhead_ != core::TimestampNs{}) {
        playhead_ = core::TimestampNs{};
        emit playheadChanged();
    }
    setStatus({});
    if (!previewImage_.isNull()) {
        previewImage_ = {};
        emit previewImageChanged();
    }
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
    setPreviewStale(true);
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
    playbackTimer_.stop();
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

void EditorController::selectClip(QString trackId, QString clipId) {
    if (!snapshot_.has_value()) {
        setStatus(QStringLiteral("No editor session is open"));
        return;
    }
    const auto track = domain::TrackId::create(
        trackId.toUtf8().toStdString());
    const auto clip = domain::ClipId::create(clipId.toUtf8().toStdString());
    if (!track.hasValue() || !clip.hasValue() ||
        snapshot_->timeline.clip(track.value(), clip.value()) == nullptr) {
        setStatus(QStringLiteral("Selected timeline clip does not exist"));
        return;
    }
    if (selectedTrackId_ == trackId && selectedClipId_ == clipId) return;
    selectedTrackId_ = std::move(trackId);
    selectedClipId_ = std::move(clipId);
    setStatus({});
    emit selectionChanged();
}

void EditorController::markRangeIn() {
    if (!snapshot_.has_value()) {
        setStatus(QStringLiteral("No editor session is open"));
        return;
    }
    core::TimestampNs in = playhead_;
    if (hasMarkedRange() && in > *rangeOut_) {
        rangeIn_ = in;
        rangeOut_.reset();
    } else if (rangeOut_.has_value() && in > *rangeOut_) {
        rangeIn_ = *rangeOut_;
        rangeOut_ = in;
    } else {
        rangeIn_ = in;
    }
    emit markedRangeChanged();
}

void EditorController::markRangeOut() {
    if (!snapshot_.has_value()) {
        setStatus(QStringLiteral("No editor session is open"));
        return;
    }
    core::TimestampNs out = playhead_;
    if (rangeIn_.has_value() && out < *rangeIn_) {
        rangeOut_ = *rangeIn_;
        rangeIn_ = out;
    } else {
        rangeOut_ = out;
    }
    emit markedRangeChanged();
}

void EditorController::openProject(QUrl projectUrl) {
    if (!projectUrl.isLocalFile() || projectUrl.toLocalFile().isEmpty()) {
        setStatus(QStringLiteral("Editor project URL must be a local file"));
        return;
    }
    ++sessionGeneration_;
    ++generation_;
    activeSessionCommand_.reset();
    durableSessionReady_ = false;
    setPlaying(false);
    if (!selectedTrackId_.isEmpty() || !selectedClipId_.isEmpty()) {
        selectedTrackId_.clear();
        selectedClipId_.clear();
        emit selectionChanged();
    }
    if (rangeIn_.has_value() || rangeOut_.has_value()) {
        rangeIn_.reset();
        rangeOut_.reset();
        emit markedRangeChanged();
    }
    if (!previewImage_.isNull()) {
        previewImage_ = {};
        emit previewImageChanged();
    }
    setPreviewStale(true);
    setSessionBusy(true);
    setStatus({});
    const auto path = pathFromQString(projectUrl.toLocalFile());
    const quint64 generation = sessionGeneration_;
    QMetaObject::invokeMethod(
        sessionWorker_,
        [worker = sessionWorker_, generation, path] {
            worker->openProject(generation, path);
        },
        Qt::QueuedConnection);
}

void EditorController::splitSelected() {
    if (selectedTrackId_.isEmpty() || selectedClipId_.isEmpty()) {
        setStatus(QStringLiteral("Select a clip before splitting"));
        return;
    }
    auto trackId = domain::TrackId::create(
        selectedTrackId_.toUtf8().toStdString());
    auto clipId = domain::ClipId::create(
        selectedClipId_.toUtf8().toStdString());
    if (!trackId.hasValue() || !clipId.hasValue()) {
        setStatus(QStringLiteral("Selected clip identity is invalid"));
        return;
    }
    queueSessionEdit(EditorEditRequest{.kind = EditorEditKind::Split,
                                       .trackId = trackId.value(),
                                       .clipId = clipId.value(),
                                       .position = playhead_});
}

void EditorController::trimSelectedStart() {
    if (selectedTrackId_.isEmpty() || selectedClipId_.isEmpty()) {
        setStatus(QStringLiteral("Select a clip before trimming"));
        return;
    }
    auto trackId = domain::TrackId::create(
        selectedTrackId_.toUtf8().toStdString());
    auto clipId = domain::ClipId::create(
        selectedClipId_.toUtf8().toStdString());
    if (!trackId.hasValue() || !clipId.hasValue()) {
        setStatus(QStringLiteral("Selected clip identity is invalid"));
        return;
    }
    queueSessionEdit(EditorEditRequest{.kind = EditorEditKind::TrimLeading,
                                       .trackId = trackId.value(),
                                       .clipId = clipId.value(),
                                       .position = playhead_});
}

void EditorController::trimSelectedEnd() {
    if (selectedTrackId_.isEmpty() || selectedClipId_.isEmpty()) {
        setStatus(QStringLiteral("Select a clip before trimming"));
        return;
    }
    auto trackId = domain::TrackId::create(
        selectedTrackId_.toUtf8().toStdString());
    auto clipId = domain::ClipId::create(
        selectedClipId_.toUtf8().toStdString());
    if (!trackId.hasValue() || !clipId.hasValue()) {
        setStatus(QStringLiteral("Selected clip identity is invalid"));
        return;
    }
    queueSessionEdit(EditorEditRequest{.kind = EditorEditKind::TrimTrailing,
                                       .trackId = trackId.value(),
                                       .clipId = clipId.value(),
                                       .position = playhead_});
}

void EditorController::deleteMarkedRange(bool ripple) {
    if (!hasMarkedRange()) {
        setStatus(QStringLiteral("Mark a non-empty range before deleting"));
        return;
    }
    auto range = domain::TimeRange::create(*rangeIn_, *rangeOut_ - *rangeIn_);
    if (!range.hasValue()) {
        setStatus(QString::fromStdString(range.error().message()));
        return;
    }
    queueSessionEdit(EditorEditRequest{.kind = EditorEditKind::DeleteRange,
                                       .range = range.value(),
                                       .ripple = ripple});
}

void EditorController::undo() {
    if (!canUndo_) {
        setStatus(QStringLiteral("There is no edit to undo"));
        return;
    }
    queueSessionEdit(EditorEditRequest{.kind = EditorEditKind::Undo});
}

void EditorController::redo() {
    if (!canRedo_) {
        setStatus(QStringLiteral("There is no edit to redo"));
        return;
    }
    queueSessionEdit(EditorEditRequest{.kind = EditorEditKind::Redo});
}

void EditorController::save() {
    queueSessionEdit(EditorEditRequest{.kind = EditorEditKind::Save});
}

void EditorController::queueSessionEdit(EditorEditRequest request) {
    if (!durableSessionReady_) {
        setStatus(QStringLiteral("No durable editor session is open"));
        return;
    }
    if (sessionBusy_) return;
    if (busy()) {
        setStatus(QStringLiteral("Wait for the preview update to finish"));
        return;
    }
    if (playing_) {
        setPlaying(false);
        queueSimple(EditorEngineOperation::Pause);
    }
    const quint64 commandId = nextSessionCommandId_++;
    activeSessionCommand_ = commandId;
    setSessionBusy(true);
    setStatus({});
    const quint64 generation = sessionGeneration_;
    QMetaObject::invokeMethod(
        sessionWorker_,
        [worker = sessionWorker_, generation, commandId,
         request = std::move(request)]() mutable {
            worker->edit(generation, commandId, std::move(request));
        },
        Qt::QueuedConnection);
}

void EditorController::handleSessionOpened(quint64 generation,
                                           EditorSessionResultPtr result) {
    if (generation != sessionGeneration_) return;
    if (!result || !result->hasValue()) {
        durableSessionReady_ = false;
        setStatus(result ? QString::fromStdString(result->error().message())
                         : QStringLiteral("Editor session returned no result"));
        setSessionBusy(false);
        return;
    }
    durableSessionReady_ = true;
    const auto& state = result->value().state;
    openSession(state.assets, state.snapshot);
    canUndo_ = state.canUndo;
    canRedo_ = state.canRedo;
    clean_ = state.clean;
    emit editStateChanged();
    setSessionBusy(false);
}

void EditorController::handleSessionEdited(quint64 generation,
                                           quint64 commandId,
                                           EditorSessionResultPtr result) {
    if (generation != sessionGeneration_ ||
        !activeSessionCommand_.has_value() ||
        commandId != *activeSessionCommand_) {
        return;
    }
    activeSessionCommand_.reset();
    if (!result || !result->hasValue()) {
        setStatus(result ? QString::fromStdString(result->error().message())
                         : QStringLiteral("Editor command returned no result"));
        setSessionBusy(false);
        return;
    }
    const auto& update = result->value();
    publishSessionState(update.state);
    if (update.change.has_value()) {
        setPreviewStale(true);
        queueUpdate(*update.change);
    }
    setSessionBusy(false);
}

void EditorController::publishSessionState(const EditorSessionState& state) {
    const bool revisionChanged =
        !snapshot_.has_value() || snapshot_->revision != state.snapshot.revision;
    mediaBinModel_.setAssets(state.assets);
    timelineTrackModel_.setTimeline(state.snapshot.timeline);
    snapshot_ = state.snapshot;
    const bool editStateChangedValue = canUndo_ != state.canUndo ||
                                       canRedo_ != state.canRedo ||
                                       clean_ != state.clean;
    canUndo_ = state.canUndo;
    canRedo_ = state.canRedo;
    clean_ = state.clean;
    if (editStateChangedValue) emit editStateChanged();

    if (!selectedTrackId_.isEmpty() && !selectedClipId_.isEmpty()) {
        auto trackId = domain::TrackId::create(
            selectedTrackId_.toUtf8().toStdString());
        auto clipId = domain::ClipId::create(
            selectedClipId_.toUtf8().toStdString());
        if (!trackId.hasValue() || !clipId.hasValue() ||
            state.snapshot.timeline.clip(trackId.value(), clipId.value()) ==
                nullptr) {
            selectedTrackId_.clear();
            selectedClipId_.clear();
            emit selectionChanged();
        } else if (revisionChanged) {
            emit selectionChanged();
        }
    }
    emit timelineChanged();
}

void EditorController::setSessionBusy(bool value) {
    if (sessionBusy_ == value) return;
    sessionBusy_ = value;
    emit sessionBusyChanged();
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

void EditorController::queueFrame(core::TimestampNs position) {
    if (!snapshot_.has_value() || previewStale_ || frameRequestInFlight_) return;
    const quint64 commandId =
        beginCommand(EditorEngineOperation::Frame, position, false,
                     snapshot_->revision.value());
    frameRequestInFlight_ = true;
    queuedCommands_.push_back(QueuedCommand{
        generation_, commandId, EditorEngineOperation::Frame, position,
        std::nullopt, std::nullopt});
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
        case EditorEngineOperation::Frame: {
            const core::TimestampNs position = *command.position;
            QMetaObject::invokeMethod(
                worker_,
                [worker = worker_, generation, commandId, position] {
                    worker->requestFrame(generation, commandId, position);
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
    std::optional<core::TimestampNs> position, bool countsAsBusy,
    std::optional<std::int64_t> expectedRevision) {
    const bool wasBusy = busy();
    const quint64 commandId = nextCommandId_++;
    commands_.emplace(commandId,
                      PendingCommand{generation_, operation, position,
                                     countsAsBusy, expectedRevision});
    if (countsAsBusy) {
        ++pendingCommands_;
        if (!wasBusy) emit busyChanged();
    }
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
    if (command.countsAsBusy) --pendingCommands_;
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
                    queueFrame(playhead_);
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
                    if (playing_) {
                        playbackStart_ = playhead_;
                        playbackClock_.restart();
                    }
                    queueFrame(playhead_);
                    break;
                case EditorEngineOperation::Update:
                    setPreviewStale(false);
                    queueFrame(playhead_);
                    break;
                case EditorEngineOperation::Frame:
                    break;
            }
        }
    }
    if (!busy()) emit busyChanged();
    dispatchNext();
}

void EditorController::handleFrameCompleted(
    quint64 generation, quint64 commandId, bool success,
    const QString& errorMessage, qlonglong revision, qlonglong positionNs,
    QImage image) {
    const auto found = commands_.find(commandId);
    if (found == commands_.end()) return;
    const PendingCommand command = found->second;
    const bool current = generation == generation_ &&
                         command.generation == generation_ &&
                         command.operation == EditorEngineOperation::Frame;
    commands_.erase(found);
    frameRequestInFlight_ = false;
    workerCommandActive_ = false;

    if (current) {
        const bool commandBecameStale =
            !snapshot_.has_value() || !command.expectedRevision.has_value() ||
            *command.expectedRevision != snapshot_->revision.value();
        const bool returnedExpectedRevision = command.expectedRevision.has_value() &&
                                              revision == *command.expectedRevision;
        const bool returnedExpectedPosition =
            command.position.has_value() &&
            positionNs == command.position->time_since_epoch().count();
        if (commandBecameStale) {
            // The durable timeline changed while the worker decoded this frame.
            // The queued update/load will request a frame for the new revision.
        } else if (!success || !returnedExpectedRevision ||
                   !returnedExpectedPosition || image.isNull()) {
            if (success && !returnedExpectedRevision) {
                setStatus(QStringLiteral(
                    "Edit engine returned the wrong preview revision"));
            } else if (success && !returnedExpectedPosition) {
                setStatus(QStringLiteral(
                    "Edit engine returned the wrong preview position"));
            } else {
                setStatus(errorMessage);
            }
            setPreviewStale(true);
            setPlaying(false);
        } else {
            previewImage_ = std::move(image);
            playhead_ = core::TimestampNs{core::DurationNs{positionNs}};
            emit previewImageChanged();
            emit playheadChanged();
        }
    }
    dispatchNext();
}

void EditorController::requestPlaybackFrame() {
    if (!playing_ || !snapshot_.has_value() || previewStale_ ||
        frameRequestInFlight_) {
        return;
    }
    const auto elapsed = core::DurationNs{playbackClock_.nsecsElapsed()};
    const auto unquantized = playbackStart_ + elapsed;
    const auto rate = snapshot_->timeline.frameRate();
    const auto frameIndex = core::timestampToFrame(unquantized, rate);
    const auto frameCount = timelineFrameCount(snapshot_->timeline);
    if (frameCount <= 0) {
        setPlaying(false);
        queueSimple(EditorEngineOperation::Pause);
        return;
    }
    if (frameIndex >= frameCount) {
        const auto lastPosition = core::frameToTimestamp(frameCount - 1, rate);
        if (lastPosition > playhead_) queueFrame(lastPosition);
        setPlaying(false);
        queueSimple(EditorEngineOperation::Pause);
        return;
    }
    const auto position = core::frameToTimestamp(frameIndex, rate);
    if (position <= playhead_) return;
    queueFrame(position);
}

void EditorController::setPreviewStale(bool value) {
    if (previewStale_ == value) return;
    previewStale_ = value;
    emit previewStaleChanged();
}

void EditorController::setPlaying(bool value) {
    if (playing_ == value) return;
    playing_ = value;
    if (playing_) {
        playbackStart_ = playhead_;
        playbackClock_.restart();
        const auto rate = snapshot_->timeline.frameRate();
        const auto interval = std::max<std::int64_t>(
            1, (1000 * rate.denominator()) / rate.numerator());
        playbackTimer_.start(static_cast<int>(interval));
    } else {
        playbackTimer_.stop();
    }
    emit playingChanged();
}

void EditorController::setStatus(QString value) {
    if (statusMessage_ == value) return;
    statusMessage_ = std::move(value);
    emit statusMessageChanged();
}

}  // namespace creator::app
