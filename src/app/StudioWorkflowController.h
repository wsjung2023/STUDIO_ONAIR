#pragma once

#include "app/StudioSceneModel.h"
#include "app/StudioSourceModel.h"
#include "app/StudioWorkflowTypes.h"

#include <QObject>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <optional>
#include <functional>

namespace creator::app {

class StudioWorkflowWorker;

class StudioWorkflowController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* sceneModel READ sceneModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* sourceModel READ sourceModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* activeSourceModel READ activeSourceModel CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(bool reconciling READ reconciling NOTIFY reconcilingChanged)
    Q_PROPERTY(QString selectedSceneId READ selectedSceneId NOTIFY selectionChanged)
    Q_PROPERTY(QString activeSceneId READ activeSceneId NOTIFY activeSceneChanged)
    Q_PROPERTY(QString selectedSourceId READ selectedSourceId NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap selectedTransform READ selectedTransform NOTIFY selectionChanged)
    Q_PROPERTY(qlonglong markerCount READ markerCount NOTIFY recordingStateChanged)
    Q_PROPERTY(qlonglong recordingPositionNs READ recordingPositionNs NOTIFY recordingStateChanged)
    Q_PROPERTY(QString activeSessionId READ activeSessionId NOTIFY recordingStateChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)

public:
    using Completion = std::function<void(core::Result<void>)>;

    StudioWorkflowController(
        StudioStoreOpenFactory storeFactory,
        std::unique_ptr<IRecordingTimelineReconciler> reconciler,
        StudioIdentityFactory identityFactory, QObject* parent = nullptr);
    ~StudioWorkflowController() override;

    [[nodiscard]] QAbstractItemModel* sceneModel() noexcept {
        return &sceneModel_;
    }
    [[nodiscard]] QAbstractItemModel* sourceModel() noexcept {
        return &sourceModel_;
    }
    [[nodiscard]] QAbstractItemModel* activeSourceModel() noexcept {
        return &activeSourceModel_;
    }
    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] bool recording() const noexcept {
        return state_.has_value() && state_->recording;
    }
    [[nodiscard]] bool reconciling() const noexcept;
    [[nodiscard]] QString selectedSceneId() const;
    [[nodiscard]] QString activeSceneId() const;
    [[nodiscard]] QString selectedSourceId() const;
    [[nodiscard]] QVariantMap selectedTransform() const;
    [[nodiscard]] qlonglong markerCount() const noexcept;
    [[nodiscard]] qlonglong recordingPositionNs() const noexcept;
    [[nodiscard]] QString activeSessionId() const;
    [[nodiscard]] QString statusMessage() const { return statusMessage_; }

    Q_INVOKABLE void openProject(QUrl packageUrl);
    Q_INVOKABLE void reopenProject();
    Q_INVOKABLE void addScene(QString name);
    Q_INVOKABLE void duplicateSelectedScene();
    Q_INVOKABLE void renameScene(QString sceneId, QString name);
    Q_INVOKABLE void removeScene(QString sceneId);
    Q_INVOKABLE void moveScene(QString sceneId, int direction);
    Q_INVOKABLE void selectScene(QString sceneId);
    Q_INVOKABLE void switchScene(QString sceneId, qlonglong positionNs = 0);
    Q_INVOKABLE void selectSource(QString sourceId);
    Q_INVOKABLE void toggleSource(QString sourceId);
    Q_INVOKABLE void moveSource(QString sourceId, int direction);
    Q_INVOKABLE void setSelectedTransform(
        double x, double y, double width, double height, double scaleX,
        double scaleY, double rotationDegrees, double cropLeft, double cropTop,
        double cropRight, double cropBottom, double opacity, int zOrder);
    Q_INVOKABLE void resetSelectedTransform();
    Q_INVOKABLE void applySelectedPipPreset(QString preset);
    Q_INVOKABLE void prepareRecording(QString sessionId, QVariantList sources,
                                       qlonglong positionNs = 0);
    Q_INVOKABLE void abortRecording();
    Q_INVOKABLE void completeRecording();
    Q_INVOKABLE void addMarker(QString label, qlonglong positionNs);
    Q_INVOKABLE void retryReconciliation();

    void openProject(QUrl packageUrl, Completion completion);
    void prepareRecording(domain::SessionId sessionId,
                          std::vector<project_store::RecordingSourceRole> sources,
                          core::TimestampNs position,
                          Completion completion);
    void abortRecording(Completion completion);
    void completeRecording(Completion completion);
    void retryReconciliation(Completion completion);

signals:
    void busyChanged();
    void recordingChanged();
    void reconcilingChanged();
    void selectionChanged();
    void activeSceneChanged();
    void recordingStateChanged();
    void statusMessageChanged();

private:
    void submit(StudioWorkflowRequest request, Completion completion = {});
    void handleCompleted(quint64 generation, quint64 commandId,
                         StudioWorkflowResultPtr result);
    void handleReconciliationProgress(quint64 generation, bool active);
    void publishState(StudioWorkflowState state);
    void setStatus(QString status);
    [[nodiscard]] const domain::StudioScene* selectedScene() const noexcept;
    [[nodiscard]] const domain::StudioScene* activeScene() const noexcept;
    [[nodiscard]] const domain::SceneSource* selectedSource() const noexcept;
    [[nodiscard]] std::optional<domain::SceneId> parsedSelectedScene() const;

    QThread workerThread_;
    StudioWorkflowWorker* worker_{};
    StudioSceneModel sceneModel_;
    StudioSourceModel sourceModel_;
    StudioSourceModel activeSourceModel_;
    std::optional<StudioWorkflowState> state_;
    std::optional<StudioWorkflowOperation> pendingOperation_;
    Completion pendingCompletion_;
    Completion openCompletion_;
    QUrl packageUrl_;
    QString statusMessage_;
    quint64 generation_{0};
    quint64 nextCommandId_{1};
    quint64 selectionVersion_{0};
    quint64 commandSelectionVersion_{0};
    bool busy_{false};
    bool workerReconciling_{false};
};

}  // namespace creator::app
