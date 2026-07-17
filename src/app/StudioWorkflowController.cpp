#include "app/StudioWorkflowController.h"

#include "app/StudioWorkflowWorker.h"
#include "core/AppError.h"

#include <QMetaObject>
#include <QString>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace creator::app {
namespace {

QString fromStd(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

std::string toStd(const QString& value) {
    const auto utf8 = value.toUtf8();
    return std::string{utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

std::filesystem::path localPath(const QUrl& value) {
    const auto utf8 = value.toLocalFile().toUtf8();
    return std::filesystem::path{std::u8string{
        reinterpret_cast<const char8_t*>(utf8.constData()),
        static_cast<std::size_t>(utf8.size())}};
}

QVariantMap transformMap(const domain::VisualTransform& value) {
    return {{QStringLiteral("x"), value.x()},
            {QStringLiteral("y"), value.y()},
            {QStringLiteral("width"), value.width()},
            {QStringLiteral("height"), value.height()},
            {QStringLiteral("scaleX"), value.scaleX()},
            {QStringLiteral("scaleY"), value.scaleY()},
            {QStringLiteral("rotationDegrees"), value.rotationDegrees()},
            {QStringLiteral("cropLeft"), value.cropLeft()},
            {QStringLiteral("cropTop"), value.cropTop()},
            {QStringLiteral("cropRight"), value.cropRight()},
            {QStringLiteral("cropBottom"), value.cropBottom()},
            {QStringLiteral("opacity"), value.opacity()},
            {QStringLiteral("zOrder"), value.zOrder()}};
}

bool isReconciliationOperation(StudioWorkflowOperation operation) noexcept {
    return operation == StudioWorkflowOperation::CompleteRecording ||
           operation == StudioWorkflowOperation::RetryReconciliation;
}

}  // namespace

StudioWorkflowController::StudioWorkflowController(
    StudioStoreOpenFactory storeFactory,
    std::unique_ptr<IRecordingTimelineReconciler> reconciler,
    StudioIdentityFactory identityFactory, QObject* parent)
    : QObject(parent), sceneModel_(this), sourceModel_(this) {
    qRegisterMetaType<StudioWorkflowResultPtr>();
    worker_ = new StudioWorkflowWorker{
        std::move(storeFactory), std::move(reconciler),
        std::move(identityFactory)};
    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &StudioWorkflowWorker::completed, this,
            &StudioWorkflowController::handleCompleted,
            Qt::QueuedConnection);
    connect(worker_, &StudioWorkflowWorker::reconciliationProgress, this,
            &StudioWorkflowController::handleReconciliationProgress,
            Qt::QueuedConnection);
    workerThread_.start();
}

StudioWorkflowController::~StudioWorkflowController() {
    ++generation_;
    workerThread_.quit();
    workerThread_.wait();
}

bool StudioWorkflowController::reconciling() const noexcept {
    return (state_.has_value() && state_->reconciling) ||
           workerReconciling_ ||
           (pendingOperation_.has_value() &&
            isReconciliationOperation(*pendingOperation_));
}

QString StudioWorkflowController::selectedSceneId() const {
    return state_.has_value() && state_->selectedSceneId.has_value()
               ? fromStd(state_->selectedSceneId->value())
               : QString{};
}

QString StudioWorkflowController::activeSceneId() const {
    return state_.has_value() ? fromStd(state_->snapshot.activeSceneId.value())
                              : QString{};
}

QString StudioWorkflowController::selectedSourceId() const {
    return state_.has_value() && state_->selectedSourceId.has_value()
               ? fromStd(state_->selectedSourceId->value())
               : QString{};
}

QVariantMap StudioWorkflowController::selectedTransform() const {
    const auto* source = selectedSource();
    return source && source->transform().has_value()
               ? transformMap(*source->transform())
               : QVariantMap{};
}

qlonglong StudioWorkflowController::markerCount() const noexcept {
    return state_.has_value()
               ? static_cast<qlonglong>(state_->markerCount)
               : 0;
}

qlonglong StudioWorkflowController::recordingPositionNs() const noexcept {
    return state_.has_value()
               ? state_->recordingPosition.time_since_epoch().count()
               : 0;
}

QString StudioWorkflowController::activeSessionId() const {
    return state_.has_value() && state_->activeSessionId.has_value()
               ? fromStd(state_->activeSessionId->value())
               : QString{};
}

void StudioWorkflowController::setStatus(QString status) {
    if (statusMessage_ == status) return;
    statusMessage_ = std::move(status);
    emit statusMessageChanged();
}

void StudioWorkflowController::openProject(QUrl packageUrl) {
    openProject(std::move(packageUrl), {});
}

void StudioWorkflowController::openProject(QUrl packageUrl,
                                           Completion completion) {
    if (!packageUrl.isLocalFile() || packageUrl.toLocalFile().isEmpty()) {
        const auto message = QStringLiteral("Studio project URL must be a local path");
        setStatus(message);
        if (completion) {
            completion(core::AppError{core::ErrorCode::InvalidArgument,
                                      toStd(message)});
        }
        return;
    }
    if (recording() || pendingOperation_.has_value()) {
        const auto message = QStringLiteral(
            "Stop the active Studio operation before opening another project");
        setStatus(message);
        if (completion) {
            completion(core::AppError{core::ErrorCode::InvalidState,
                                      toStd(message)});
        }
        return;
    }
    packageUrl_ = std::move(packageUrl);
    ++generation_;
    const auto generation = generation_;
    const bool wasRecording = recording();
    const bool hadSelection = !selectedSceneId().isEmpty() ||
                              !selectedSourceId().isEmpty();
    const bool hadActiveScene = !activeSceneId().isEmpty();
    const bool hadRecordingState = wasRecording || markerCount() != 0 ||
                                   recordingPositionNs() != 0 ||
                                   !activeSessionId().isEmpty();
    state_.reset();
    ++selectionVersion_;
    sceneModel_.setScenes({}, std::nullopt, std::nullopt);
    sourceModel_.clear();
    if (wasRecording) emit recordingChanged();
    if (hadSelection) emit selectionChanged();
    if (hadActiveScene) emit activeSceneChanged();
    if (hadRecordingState) emit recordingStateChanged();
    pendingOperation_.reset();
    pendingCompletion_ = {};
    openCompletion_ = std::move(completion);
    if (workerReconciling_) {
        workerReconciling_ = false;
        emit reconcilingChanged();
    }
    if (!busy_) {
        busy_ = true;
        emit busyChanged();
    }
    setStatus(QStringLiteral("Opening Studio project"));
    const auto path = localPath(packageUrl_);
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation, path] {
            worker->openProject(generation, path);
        },
        Qt::QueuedConnection);
}

void StudioWorkflowController::reopenProject() {
    if (packageUrl_.isEmpty()) {
        setStatus(QStringLiteral("No Studio project has been opened"));
        return;
    }
    openProject(packageUrl_);
}

void StudioWorkflowController::submit(StudioWorkflowRequest request,
                                      Completion completion) {
    if (!state_.has_value()) {
        const auto message = QStringLiteral("Studio project is not ready");
        setStatus(message);
        if (completion) {
            completion(core::AppError{core::ErrorCode::InvalidState,
                                      toStd(message)});
        }
        return;
    }
    if (busy_) {
        const auto message = QStringLiteral("Studio operation is already in progress");
        setStatus(message);
        if (completion) {
            completion(core::AppError{core::ErrorCode::InvalidState,
                                      toStd(message)});
        }
        return;
    }
    busy_ = true;
    pendingOperation_ = request.operation;
    pendingCompletion_ = std::move(completion);
    commandSelectionVersion_ = selectionVersion_;
    emit busyChanged();
    if (isReconciliationOperation(request.operation)) emit reconcilingChanged();
    const auto generation = generation_;
    const auto commandId = nextCommandId_++;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation, commandId,
         request = std::move(request)]() mutable {
            worker->execute(generation, commandId, std::move(request));
        },
        Qt::QueuedConnection);
}

void StudioWorkflowController::handleCompleted(
    quint64 generation, quint64, StudioWorkflowResultPtr result) {
    if (generation != generation_ || !result) return;
    const bool wasReconciling = reconciling();
    const auto completedOperation = pendingOperation_;
    auto completion = completedOperation.has_value()
                          ? std::move(pendingCompletion_)
                          : std::move(openCompletion_);
    const bool selectionChangedDuringCommand =
        selectionVersion_ != commandSelectionVersion_;
    busy_ = false;
    pendingOperation_.reset();
    pendingCompletion_ = {};
    openCompletion_ = {};
    emit busyChanged();
    if (!result->hasValue()) {
        setStatus(fromStd(result->error().message()));
        if (wasReconciling) emit reconcilingChanged();
        if (completion) completion(result->error());
        return;
    }
    auto next = result->value();
    const bool workerChoosesSelection =
        !selectionChangedDuringCommand &&
        (completedOperation == StudioWorkflowOperation::AddScene ||
         completedOperation == StudioWorkflowOperation::DuplicateScene ||
         completedOperation == StudioWorkflowOperation::RemoveScene);
    if (!workerChoosesSelection && state_.has_value() &&
        state_->selectedSceneId.has_value()) {
        const auto scene = std::ranges::find(next.snapshot.scenes,
                                              *state_->selectedSceneId,
                                              &domain::StudioScene::id);
        if (scene != next.snapshot.scenes.end()) {
            next.selectedSceneId = state_->selectedSceneId;
            if (state_->selectedSourceId.has_value() &&
                std::ranges::find(scene->sources(),
                                  *state_->selectedSourceId,
                                  &domain::SceneSource::id) !=
                    scene->sources().end()) {
                next.selectedSourceId = state_->selectedSourceId;
            } else {
                next.selectedSourceId.reset();
            }
        }
    }
    publishState(std::move(next));
    if (wasReconciling != reconciling()) emit reconcilingChanged();
    if (completion) {
        const bool reconciliationFailed =
            (!completedOperation.has_value() ||
             isReconciliationOperation(*completedOperation)) &&
            (state_->reconciliationIncomplete ||
             state_->reconciliationSessionId.has_value());
        if (reconciliationFailed) {
            completion(core::AppError{
                core::ErrorCode::IoFailure,
                state_->status.empty()
                    ? std::string{"timeline reconciliation remains pending"}
                    : state_->status});
        } else {
            completion(core::ok());
        }
    }
}

void StudioWorkflowController::handleReconciliationProgress(
    quint64 generation, bool active) {
    if (generation != generation_ || workerReconciling_ == active) return;
    const bool wasReconciling = reconciling();
    workerReconciling_ = active;
    if (wasReconciling != reconciling()) emit reconcilingChanged();
}

void StudioWorkflowController::publishState(StudioWorkflowState state) {
    const bool oldRecording = recording();
    const QString oldActive = activeSceneId();
    const QString oldSelectedScene = selectedSceneId();
    const QString oldSelectedSource = selectedSourceId();
    const QVariantMap oldTransform = selectedTransform();
    const qlonglong oldMarkers = markerCount();
    const qlonglong oldPosition = recordingPositionNs();
    const QString oldSession = activeSessionId();
    state_ = std::move(state);
    sceneModel_.setScenes(state_->snapshot.scenes,
                          state_->snapshot.activeSceneId,
                          state_->selectedSceneId);
    const auto* scene = selectedScene();
    if (scene) {
        sourceModel_.setScene(*scene, state_->selectedSourceId);
    } else {
        sourceModel_.clear();
    }
    setStatus(fromStd(state_->status));
    if (oldRecording != recording()) emit recordingChanged();
    if (oldActive != activeSceneId()) emit activeSceneChanged();
    if (oldSelectedScene != selectedSceneId() ||
        oldSelectedSource != selectedSourceId() ||
        oldTransform != selectedTransform()) {
        emit selectionChanged();
    }
    if (oldMarkers != markerCount() || oldPosition != recordingPositionNs() ||
        oldSession != activeSessionId() || oldRecording != recording()) {
        emit recordingStateChanged();
    }
}

const domain::StudioScene* StudioWorkflowController::selectedScene() const noexcept {
    if (!state_.has_value() || !state_->selectedSceneId.has_value()) return nullptr;
    const auto found = std::ranges::find(state_->snapshot.scenes,
                                         *state_->selectedSceneId,
                                         &domain::StudioScene::id);
    return found == state_->snapshot.scenes.end() ? nullptr : &*found;
}

const domain::SceneSource* StudioWorkflowController::selectedSource() const noexcept {
    const auto* scene = selectedScene();
    if (!scene || !state_->selectedSourceId.has_value()) return nullptr;
    const auto found = std::ranges::find(scene->sources(),
                                         *state_->selectedSourceId,
                                         &domain::SceneSource::id);
    return found == scene->sources().end() ? nullptr : &*found;
}

std::optional<domain::SceneId>
StudioWorkflowController::parsedSelectedScene() const {
    return state_.has_value() ? state_->selectedSceneId
                              : std::optional<domain::SceneId>{};
}

void StudioWorkflowController::addScene(QString name) {
    submit(StudioWorkflowRequest{.operation = StudioWorkflowOperation::AddScene,
                                 .text = toStd(name)});
}

void StudioWorkflowController::duplicateSelectedScene() {
    const auto scene = parsedSelectedScene();
    submit(StudioWorkflowRequest{
        .operation = StudioWorkflowOperation::DuplicateScene,
        .sceneId = scene.has_value() ? scene->value() : std::string{}});
}

void StudioWorkflowController::renameScene(QString sceneId, QString name) {
    submit(StudioWorkflowRequest{.operation = StudioWorkflowOperation::RenameScene,
                                 .sceneId = toStd(sceneId),
                                 .text = toStd(name)});
}

void StudioWorkflowController::removeScene(QString sceneId) {
    submit(StudioWorkflowRequest{.operation = StudioWorkflowOperation::RemoveScene,
                                 .sceneId = toStd(sceneId)});
}

void StudioWorkflowController::moveScene(QString sceneId, int direction) {
    submit(StudioWorkflowRequest{.operation = StudioWorkflowOperation::MoveScene,
                                 .sceneId = toStd(sceneId),
                                 .direction = direction});
}

void StudioWorkflowController::selectScene(QString sceneId) {
    if (!state_.has_value()) {
        setStatus(QStringLiteral("Studio scene cannot be selected now"));
        return;
    }
    auto parsed = domain::SceneId::create(toStd(sceneId));
    if (!parsed.hasValue()) {
        setStatus(fromStd(parsed.error().message()));
        return;
    }
    const auto found = std::ranges::find(state_->snapshot.scenes, parsed.value(),
                                         &domain::StudioScene::id);
    if (found == state_->snapshot.scenes.end()) {
        setStatus(QStringLiteral("Studio scene was not found"));
        return;
    }
    auto staged = *state_;
    ++selectionVersion_;
    staged.selectedSceneId = parsed.value();
    staged.selectedSourceId = found->sources().empty()
                                  ? std::optional<domain::SourceId>{}
                                  : std::optional<domain::SourceId>{
                                        found->sources().front().id()};
    publishState(std::move(staged));
}

void StudioWorkflowController::switchScene(QString sceneId,
                                           qlonglong positionNs) {
    submit(StudioWorkflowRequest{.operation = StudioWorkflowOperation::SwitchScene,
                                 .sceneId = toStd(sceneId),
                                 .position = core::TimestampNs{
                                     core::DurationNs{positionNs}}});
}

void StudioWorkflowController::selectSource(QString sourceId) {
    if (!state_.has_value()) {
        setStatus(QStringLiteral("Studio source cannot be selected now"));
        return;
    }
    auto parsed = domain::SourceId::create(toStd(sourceId));
    const auto* scene = selectedScene();
    if (!parsed.hasValue() || !scene ||
        std::ranges::find(scene->sources(), parsed.value(),
                          &domain::SceneSource::id) == scene->sources().end()) {
        setStatus(QStringLiteral("Studio source was not found"));
        return;
    }
    auto staged = *state_;
    ++selectionVersion_;
    staged.selectedSourceId = parsed.value();
    publishState(std::move(staged));
}

void StudioWorkflowController::toggleSource(QString sourceId) {
    const auto scene = parsedSelectedScene();
    submit(StudioWorkflowRequest{
        .operation = StudioWorkflowOperation::ToggleSource,
        .sceneId = scene.has_value() ? scene->value() : std::string{},
        .sourceId = toStd(sourceId)});
}

void StudioWorkflowController::moveSource(QString sourceId, int direction) {
    const auto scene = parsedSelectedScene();
    submit(StudioWorkflowRequest{
        .operation = StudioWorkflowOperation::MoveSource,
        .sceneId = scene.has_value() ? scene->value() : std::string{},
        .sourceId = toStd(sourceId),
        .direction = direction});
}

void StudioWorkflowController::setSelectedTransform(
    double x, double y, double width, double height, double scaleX,
    double scaleY, double rotationDegrees, double cropLeft, double cropTop,
    double cropRight, double cropBottom, double opacity, int zOrder) {
    const auto* source = selectedSource();
    const auto scene = parsedSelectedScene();
    if (!source || !scene.has_value()) {
        setStatus(QStringLiteral("No Studio source is selected"));
        return;
    }
    auto transform = domain::VisualTransform::create(
        x, y, width, height, scaleX, scaleY, rotationDegrees, cropLeft,
        cropTop, cropRight, cropBottom, opacity, zOrder);
    if (!transform.hasValue()) {
        setStatus(fromStd(transform.error().message()));
        return;
    }
    submit(StudioWorkflowRequest{
        .operation = StudioWorkflowOperation::SetSourceTransform,
        .sceneId = scene->value(),
        .sourceId = source->id().value(),
        .transform = std::move(transform).value()});
}

void StudioWorkflowController::resetSelectedTransform() {
    const auto* source = selectedSource();
    const auto scene = parsedSelectedScene();
    if (!source || !scene.has_value()) {
        setStatus(QStringLiteral("No Studio source is selected"));
        return;
    }
    submit(StudioWorkflowRequest{
        .operation = StudioWorkflowOperation::ResetSourceTransform,
        .sceneId = scene->value(),
        .sourceId = source->id().value()});
}

void StudioWorkflowController::applySelectedPipPreset(QString preset) {
    const auto* source = selectedSource();
    const auto scene = parsedSelectedScene();
    if (!source || !scene.has_value()) {
        setStatus(QStringLiteral("No Studio source is selected"));
        return;
    }
    submit(StudioWorkflowRequest{
        .operation = StudioWorkflowOperation::SetSourcePipPreset,
        .sceneId = scene->value(),
        .sourceId = source->id().value(),
        .text = toStd(preset)});
}

void StudioWorkflowController::prepareRecording(QString sessionId,
                                                QVariantList sources,
                                                qlonglong positionNs) {
    auto parsed = domain::SessionId::create(toStd(sessionId));
    if (!parsed.hasValue()) {
        setStatus(fromStd(parsed.error().message()));
        return;
    }
    std::vector<project_store::RecordingSourceRole> roles;
    roles.reserve(static_cast<std::size_t>(sources.size()));
    for (const auto& item : sources) {
        const auto map = item.toMap();
        auto source = domain::SourceId::create(
            toStd(map.value(QStringLiteral("sourceId")).toString()));
        auto role = domain::studioSourceRoleFromName(
            toStd(map.value(QStringLiteral("role")).toString()));
        if (!source.hasValue() || !role.hasValue()) {
            setStatus(QStringLiteral("Recording source mapping is invalid"));
            return;
        }
        roles.push_back({.sourceId = std::move(source).value(),
                         .role = role.value()});
    }
    submit(StudioWorkflowRequest{
        .operation = StudioWorkflowOperation::PrepareRecording,
        .sessionId = std::move(parsed).value(),
        .recordingSources = std::move(roles),
        .position = core::TimestampNs{core::DurationNs{positionNs}}});
}

void StudioWorkflowController::prepareRecording(
    domain::SessionId sessionId,
    std::vector<project_store::RecordingSourceRole> sources,
    core::TimestampNs position, Completion completion) {
    submit(StudioWorkflowRequest{
               .operation = StudioWorkflowOperation::PrepareRecording,
               .sessionId = std::move(sessionId),
               .recordingSources = std::move(sources),
               .position = position},
           std::move(completion));
}

void StudioWorkflowController::abortRecording() {
    submit(StudioWorkflowRequest{
        .operation = StudioWorkflowOperation::AbortRecording});
}

void StudioWorkflowController::abortRecording(Completion completion) {
    submit(StudioWorkflowRequest{
               .operation = StudioWorkflowOperation::AbortRecording},
           std::move(completion));
}

void StudioWorkflowController::completeRecording() {
    submit(StudioWorkflowRequest{
        .operation = StudioWorkflowOperation::CompleteRecording});
}

void StudioWorkflowController::completeRecording(Completion completion) {
    submit(StudioWorkflowRequest{
               .operation = StudioWorkflowOperation::CompleteRecording},
           std::move(completion));
}

void StudioWorkflowController::addMarker(QString label,
                                         qlonglong positionNs) {
    submit(StudioWorkflowRequest{
        .operation = StudioWorkflowOperation::AddMarker,
        .text = toStd(label),
        .position = core::TimestampNs{core::DurationNs{positionNs}}});
}

void StudioWorkflowController::retryReconciliation() {
    submit(StudioWorkflowRequest{
        .operation = StudioWorkflowOperation::RetryReconciliation});
}

void StudioWorkflowController::retryReconciliation(Completion completion) {
    submit(StudioWorkflowRequest{
               .operation = StudioWorkflowOperation::RetryReconciliation},
           std::move(completion));
}

}  // namespace creator::app
