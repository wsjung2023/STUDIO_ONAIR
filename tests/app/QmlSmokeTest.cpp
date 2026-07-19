#include "app/ScreenPreviewItem.h"
#include "app/CameraPreviewItem.h"
#include "app/EditorPreviewItem.h"
#include "app/MediaBinModel.h"
#include "app/TimelineTrackModel.h"

#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/MediaAsset.h"
#include "domain/Timeline.h"
#include "domain/TimelineRevision.h"
#include "domain/TimelineTypes.h"

#include <QGuiApplication>
#include <QAccessible>
#include <QImage>
#include <QCoreApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <qqml.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

namespace {

QQuickItem* findVisualItem(QQuickItem* root, const QString& objectName) {
    if (root->objectName() == objectName) return root;
    for (QQuickItem* child : root->childItems()) {
        if (auto* found = findVisualItem(child, objectName); found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

class FakeProjectController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy CONSTANT)
    Q_PROPERTY(bool hasOpenProject READ hasOpenProject NOTIFY projectStateChanged)
    Q_PROPERTY(QString projectName READ projectName CONSTANT)
    Q_PROPERTY(QUrl projectUrl READ projectUrl CONSTANT)
    Q_PROPERTY(QVariantList recentProjects READ recentProjects CONSTANT)
    Q_PROPERTY(QVariantList recoveries READ recoveries CONSTANT)
    Q_PROPERTY(QString statusMessage READ statusMessage CONSTANT)

public:
    using QObject::QObject;

    [[nodiscard]] bool busy() const noexcept { return false; }
    [[nodiscard]] bool hasOpenProject() const noexcept { return hasOpenProject_; }
    [[nodiscard]] QString projectName() const { return {}; }
    [[nodiscard]] QUrl projectUrl() const { return {}; }
    [[nodiscard]] QVariantList recentProjects() const { return {}; }
    [[nodiscard]] QVariantList recoveries() const {
        return {QVariantMap{
            {QStringLiteral("sessionId"), QStringLiteral("session-1")},
            {QStringLiteral("projectName"), QString::fromUtf8("강의")},
            {QStringLiteral("projectUrl"), QUrl::fromLocalFile("C:/fixture.cstudio")},
            {QStringLiteral("createdAt"), QStringLiteral("2026-07-16T12:00:00Z")},
            {QStringLiteral("readySegments"), 1},
            {QStringLiteral("writingSegments"), 1},
        }};
    }
    [[nodiscard]] QString statusMessage() const { return {}; }

    Q_INVOKABLE void recoverSession(const QString&) {}
    Q_INVOKABLE void leaveRecoveryForLater() {}
    Q_INVOKABLE void createProject(const QUrl&, const QString&) {}
    Q_INVOKABLE void openProject(const QUrl&) {}
    void setHasOpenProject(bool open) {
        if (hasOpenProject_ == open) return;
        hasOpenProject_ = open;
        emit projectStateChanged();
    }

signals:
    void projectOpened();
    void recoveryRequired();
    void recoveryDeferred();
    void projectStateChanged();

private:
    bool hasOpenProject_{false};
};

class FakeExportController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool canCancel READ canCancel NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(int state READ state NOTIFY changed)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(quint32 maximumExportHeight READ maximumExportHeight NOTIFY changed)
    Q_PROPERTY(bool foregroundExportRequired READ foregroundExportRequired NOTIFY changed)
    Q_PROPERTY(bool exportAllowed READ exportAllowed NOTIFY changed)
    Q_PROPERTY(bool applicationActive READ applicationActive NOTIFY changed)

public:
    using QObject::QObject;
    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] bool canCancel() const noexcept { return busy_; }
    [[nodiscard]] double progress() const noexcept { return busy_ ? 0.4 : 0.0; }
    [[nodiscard]] int state() const noexcept { return busy_ ? 1 : 0; }
    [[nodiscard]] QString statusMessage() const {
        return busy_ ? QStringLiteral("Exporting") : QString{};
    }
    [[nodiscard]] bool ready() const noexcept { return true; }
    [[nodiscard]] quint32 maximumExportHeight() const noexcept {
        return maximumExportHeight_;
    }
    [[nodiscard]] bool foregroundExportRequired() const noexcept { return true; }
    [[nodiscard]] bool exportAllowed() const noexcept { return exportAllowed_; }
    [[nodiscard]] bool applicationActive() const noexcept { return applicationActive_; }
    Q_INVOKABLE void exportTo(const QUrl&, const QString&, bool) {
        ++exportCalls_;
    }
    Q_INVOKABLE void cancelExport() { ++cancelCalls_; }
    void setBusy(bool value) {
        busy_ = value;
        emit changed();
    }
    void setMaximumExportHeight(quint32 value) {
        maximumExportHeight_ = value;
        emit changed();
    }
    void setExportAllowed(bool value) {
        exportAllowed_ = value;
        emit changed();
    }
    [[nodiscard]] int exportCalls() const noexcept { return exportCalls_; }
    [[nodiscard]] int cancelCalls() const noexcept { return cancelCalls_; }

signals:
    void changed();

private:
    bool busy_{};
    quint32 maximumExportHeight_{2'160};
    bool exportAllowed_{true};
    bool applicationActive_{true};
    int exportCalls_{};
    int cancelCalls_{};
};

class FakeStudioController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY stateChanged)
    Q_PROPERTY(bool recordingAvailable READ recordingAvailable CONSTANT)
    Q_PROPERTY(int segmentCount READ segmentCount CONSTANT)
    Q_PROPERTY(int trackCount READ trackCount CONSTANT)
    Q_PROPERTY(qulonglong queuedItems READ queuedItems CONSTANT)
    Q_PROPERTY(qulonglong droppedFrames READ droppedFrames CONSTANT)
    Q_PROPERTY(qulonglong syncDroppedFrames READ syncDroppedFrames CONSTANT)
    Q_PROPERTY(qulonglong duplicatedFrames READ duplicatedFrames CONSTANT)
    Q_PROPERTY(double maximumDriftMilliseconds READ maximumDriftMilliseconds CONSTANT)
    Q_PROPERTY(double audioCorrectionPpm READ audioCorrectionPpm CONSTANT)
    Q_PROPERTY(qulonglong diskAvailableBytes READ diskAvailableBytes CONSTANT)
    Q_PROPERTY(QString encoderName READ encoderName CONSTANT)
    Q_PROPERTY(QString takeDuration READ takeDuration CONSTANT)
    Q_PROPERTY(QString statusMessage READ statusMessage CONSTANT)
    Q_PROPERTY(qlonglong recordingPositionNs READ recordingPositionNs CONSTANT)

public:
    using QObject::QObject;
    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] bool recording() const noexcept { return recording_; }
    [[nodiscard]] bool recordingAvailable() const noexcept { return true; }
    [[nodiscard]] int segmentCount() const noexcept { return 1; }
    [[nodiscard]] int trackCount() const noexcept { return 2; }
    [[nodiscard]] qulonglong queuedItems() const noexcept { return 3; }
    [[nodiscard]] qulonglong droppedFrames() const noexcept { return 1; }
    [[nodiscard]] qulonglong syncDroppedFrames() const noexcept { return 2; }
    [[nodiscard]] qulonglong duplicatedFrames() const noexcept { return 4; }
    [[nodiscard]] double maximumDriftMilliseconds() const noexcept { return 8.5; }
    [[nodiscard]] double audioCorrectionPpm() const noexcept { return 125.0; }
    [[nodiscard]] qulonglong diskAvailableBytes() const noexcept {
        return 8ULL * 1024ULL * 1024ULL * 1024ULL;
    }
    [[nodiscard]] QString encoderName() const { return QStringLiteral("mpeg4, aac"); }
    [[nodiscard]] QString takeDuration() const { return QStringLiteral("00:00:00"); }
    [[nodiscard]] QString statusMessage() const { return {}; }
    [[nodiscard]] qlonglong recordingPositionNs() const noexcept { return 777; }
    Q_INVOKABLE void startRecording() {
        ++startCalls_;
        setRecording(true);
    }
    Q_INVOKABLE void stopRecording() {
        ++stopCalls_;
        setRecording(false);
    }
    void setBusy(bool busy) {
        if (busy_ == busy) return;
        busy_ = busy;
        emit stateChanged();
    }
    void setRecording(bool recording) {
        if (recording_ == recording) return;
        recording_ = recording;
        emit stateChanged();
    }
    [[nodiscard]] int startCalls() const noexcept { return startCalls_; }
    [[nodiscard]] int stopCalls() const noexcept { return stopCalls_; }

signals:
    void stateChanged();

private:
    bool busy_{false};
    bool recording_{false};
    int startCalls_{0};
    int stopCalls_{0};
};

class FakeShortcutSettingsController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString recordShortcut READ recordShortcut CONSTANT)
    Q_PROPERTY(QString markerShortcut READ markerShortcut CONSTANT)
    Q_PROPERTY(QString previousSceneShortcut READ previousSceneShortcut CONSTANT)
    Q_PROPERTY(QString nextSceneShortcut READ nextSceneShortcut CONSTANT)
    Q_PROPERTY(QString scene1Shortcut READ scene1Shortcut CONSTANT)
    Q_PROPERTY(QString scene2Shortcut READ scene2Shortcut CONSTANT)
    Q_PROPERTY(QString scene3Shortcut READ scene3Shortcut CONSTANT)
    Q_PROPERTY(QString scene4Shortcut READ scene4Shortcut CONSTANT)
    Q_PROPERTY(QString scene5Shortcut READ scene5Shortcut CONSTANT)
    Q_PROPERTY(QString scene6Shortcut READ scene6Shortcut CONSTANT)
    Q_PROPERTY(QString scene7Shortcut READ scene7Shortcut CONSTANT)
    Q_PROPERTY(QString scene8Shortcut READ scene8Shortcut CONSTANT)
    Q_PROPERTY(QString scene9Shortcut READ scene9Shortcut CONSTANT)
    Q_PROPERTY(QString statusMessage READ statusMessage CONSTANT)

public:
    using QObject::QObject;
    [[nodiscard]] QString recordShortcut() const { return QStringLiteral("Ctrl+Shift+R"); }
    [[nodiscard]] QString markerShortcut() const { return QStringLiteral("M"); }
    [[nodiscard]] QString previousSceneShortcut() const { return QStringLiteral("Ctrl+PgUp"); }
    [[nodiscard]] QString nextSceneShortcut() const { return QStringLiteral("Ctrl+PgDown"); }
    [[nodiscard]] QString scene1Shortcut() const { return QStringLiteral("Ctrl+1"); }
    [[nodiscard]] QString scene2Shortcut() const { return QStringLiteral("Ctrl+2"); }
    [[nodiscard]] QString scene3Shortcut() const { return QStringLiteral("Ctrl+3"); }
    [[nodiscard]] QString scene4Shortcut() const { return QStringLiteral("Ctrl+4"); }
    [[nodiscard]] QString scene5Shortcut() const { return QStringLiteral("Ctrl+5"); }
    [[nodiscard]] QString scene6Shortcut() const { return QStringLiteral("Ctrl+6"); }
    [[nodiscard]] QString scene7Shortcut() const { return QStringLiteral("Ctrl+7"); }
    [[nodiscard]] QString scene8Shortcut() const { return QStringLiteral("Ctrl+8"); }
    [[nodiscard]] QString scene9Shortcut() const { return QStringLiteral("Ctrl+9"); }
    [[nodiscard]] QString statusMessage() const { return {}; }
    Q_INVOKABLE void setShortcut(const QString& actionId, const QString& sequence) {
        ++setCalls_;
        lastActionId_ = actionId;
        lastSequence_ = sequence;
    }
    [[nodiscard]] int setCalls() const noexcept { return setCalls_; }

private:
    int setCalls_{0};
    QString lastActionId_;
    QString lastSequence_;
};

class FakeStudioSceneModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(qulonglong revision READ revision NOTIFY revisionChanged)

public:
    enum Role {
        SceneIdRole = Qt::UserRole + 1,
        NameRole,
        PositionRole,
        ActiveRole,
        SelectedRole,
        SourceCountRole,
    };
    explicit FakeStudioSceneModel(QObject* parent = nullptr)
        : QAbstractListModel(parent) {
        for (int index = 1; index <= 9; ++index) {
            sceneIds_.push_back(QStringLiteral("scene-%1").arg(index));
        }
    }

    [[nodiscard]] int rowCount(
        const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : static_cast<int>(sceneIds_.size());
    }
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 ||
            index.row() >= static_cast<int>(sceneIds_.size())) {
            return {};
        }
        const QString& id = sceneIds_[static_cast<std::size_t>(index.row())];
        switch (role) {
        case SceneIdRole: return id;
        case NameRole: return QStringLiteral("Scene %1").arg(index.row() + 1);
        case PositionRole: return index.row();
        case ActiveRole: return id == QStringLiteral("scene-5");
        case SelectedRole: return id == QStringLiteral("scene-1");
        case SourceCountRole: return 2;
        default: return {};
        }
    }
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override {
        return {{SceneIdRole, "sceneId"}, {NameRole, "name"},
                {PositionRole, "position"}, {ActiveRole, "active"},
                {SelectedRole, "selected"}, {SourceCountRole, "sourceCount"}};
    }
    Q_INVOKABLE QString sceneIdAt(int row) const {
        if (row < 0 || row >= static_cast<int>(sceneIds_.size())) return {};
        return sceneIds_[static_cast<std::size_t>(row)];
    }
    [[nodiscard]] qulonglong revision() const noexcept { return revision_; }
    void setSceneCount(int count) {
        beginResetModel();
        sceneIds_.clear();
        for (int index = 1; index <= count; ++index) {
            sceneIds_.push_back(QStringLiteral("scene-%1").arg(index));
        }
        endResetModel();
        ++revision_;
        emit revisionChanged();
    }

signals:
    void revisionChanged();

private:
    std::vector<QString> sceneIds_;
    qulonglong revision_{1};
};

class FakeStudioSourceModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(qulonglong revision READ revision CONSTANT)

public:
    enum Role {
        SourceIdRole = Qt::UserRole + 1,
        NameRole,
        RoleNameRole,
        PositionRole,
        EnabledRole,
        SelectedRole,
        HasTransformRole,
        TransformRole,
        SourceEnabledRole,
    };
    explicit FakeStudioSourceModel(bool activeComposition = false,
                                   QObject* parent = nullptr)
        : QAbstractListModel(parent), activeComposition_(activeComposition) {}
    [[nodiscard]] int rowCount(
        const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : 2;
    }
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= 2) return {};
        const bool camera = index.row() == 1;
        const QVariantMap transform{
            {QStringLiteral("x"), camera ? (activeComposition_ ? 0.61 : 0.70) : 0.0},
            {QStringLiteral("y"), camera ? 0.70 : 0.0},
            {QStringLiteral("width"), camera ? 0.25 : 1.0},
            {QStringLiteral("height"), camera ? 0.25 : 1.0},
            {QStringLiteral("scaleX"), camera && activeComposition_ ? 1.2 : 1.0},
            {QStringLiteral("scaleY"), camera && activeComposition_ ? 0.8 : 1.0},
            {QStringLiteral("rotationDegrees"), 0.0},
            {QStringLiteral("cropLeft"), camera && activeComposition_ ? 0.1 : 0.0},
            {QStringLiteral("cropTop"), camera && activeComposition_ ? 0.05 : 0.0},
            {QStringLiteral("cropRight"), camera && activeComposition_ ? 0.2 : 0.0},
            {QStringLiteral("cropBottom"), camera && activeComposition_ ? 0.15 : 0.0},
            {QStringLiteral("opacity"), 1.0},
            {QStringLiteral("zOrder"), camera ? 10 : 0}};
        switch (role) {
        case SourceIdRole: return camera ? QStringLiteral("camera") : QStringLiteral("screen");
        case NameRole: return camera ? QStringLiteral("Camera") : QStringLiteral("Screen");
        case RoleNameRole: return camera ? QStringLiteral("camera") : QStringLiteral("screen");
        case PositionRole: return index.row();
        case EnabledRole: return true;
        case SourceEnabledRole: return true;
        case SelectedRole: return camera;
        case HasTransformRole: return true;
        case TransformRole: return transform;
        default: return {};
        }
    }
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override {
        return {{SourceIdRole, "sourceId"}, {NameRole, "name"},
                {RoleNameRole, "role"}, {PositionRole, "position"},
                {EnabledRole, "enabled"}, {SelectedRole, "selected"},
                {HasTransformRole, "hasTransform"}, {TransformRole, "transform"},
                {SourceEnabledRole, "sourceEnabled"}};
    }
    [[nodiscard]] qulonglong revision() const noexcept { return 1; }
    Q_INVOKABLE bool enabledForRole(const QString& roleName) const {
        return roleName == QStringLiteral("screen") ||
               roleName == QStringLiteral("camera");
    }
    Q_INVOKABLE QVariantMap transformForRole(const QString& roleName) const {
        const int row = roleName == QStringLiteral("camera") ? 1 : 0;
        return data(index(row, 0), TransformRole).toMap();
    }

private:
    bool activeComposition_{false};
};

class FakeStudioWorkflowController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* sceneModel READ sceneModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* sourceModel READ sourceModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* activeSourceModel READ activeSourceModel CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY stateChanged)
    Q_PROPERTY(QString activeSceneId READ activeSceneId NOTIFY stateChanged)
    Q_PROPERTY(QString selectedSceneId READ selectedSceneId NOTIFY stateChanged)
    Q_PROPERTY(QString selectedSourceId READ selectedSourceId NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap selectedTransform READ selectedTransform NOTIFY stateChanged)
    Q_PROPERTY(qlonglong markerCount READ markerCount NOTIFY stateChanged)
    Q_PROPERTY(QString activeSessionId READ activeSessionId NOTIFY stateChanged)
    Q_PROPERTY(bool reconciling READ reconciling NOTIFY stateChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY stateChanged)
    Q_PROPERTY(qlonglong recordingPositionNs READ recordingPositionNs CONSTANT)

public:
    explicit FakeStudioWorkflowController(QObject* parent = nullptr)
        : QObject(parent), scenes_(this), sources_(false, this),
          activeSources_(true, this) {}

    [[nodiscard]] QAbstractItemModel* sceneModel() noexcept { return &scenes_; }
    [[nodiscard]] QAbstractItemModel* sourceModel() noexcept { return &sources_; }
    [[nodiscard]] QAbstractItemModel* activeSourceModel() noexcept {
        return &activeSources_;
    }
    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] bool recording() const noexcept { return recording_; }
    [[nodiscard]] QString activeSceneId() const { return activeSceneId_; }
    [[nodiscard]] QString selectedSceneId() const { return QStringLiteral("scene-1"); }
    [[nodiscard]] QString selectedSourceId() const { return selectedSourceId_; }
    [[nodiscard]] QVariantMap selectedTransform() const {
        if (selectedSourceId_ != QStringLiteral("camera") &&
            selectedSourceId_ != QStringLiteral("screen")) {
            return {};
        }
        return sources_.data(sources_.index(1, 0),
                             FakeStudioSourceModel::TransformRole).toMap();
    }
    [[nodiscard]] qlonglong markerCount() const noexcept { return 3; }
    [[nodiscard]] QString activeSessionId() const { return QStringLiteral("session-abcdef"); }
    [[nodiscard]] bool reconciling() const noexcept { return false; }
    [[nodiscard]] QString statusMessage() const { return {}; }
    [[nodiscard]] qlonglong recordingPositionNs() const noexcept { return 500; }
    Q_INVOKABLE void switchScene(const QString& sceneId, qlonglong positionNs) {
        ++switchCalls_;
        lastSceneId_ = sceneId;
        lastPositionNs_ = positionNs;
        activeSceneId_ = sceneId;
        emit stateChanged();
    }
    Q_INVOKABLE void addMarker(const QString&, qlonglong positionNs) {
        ++markerCalls_;
        lastPositionNs_ = positionNs;
    }
    Q_INVOKABLE void addScene(const QString&) { ++sceneEditCalls_; }
    Q_INVOKABLE void duplicateSelectedScene() { ++sceneEditCalls_; }
    Q_INVOKABLE void renameScene(const QString&, const QString&) {
        ++sceneEditCalls_;
    }
    Q_INVOKABLE void removeScene(const QString&) { ++sceneEditCalls_; }
    Q_INVOKABLE void moveScene(const QString&, int) { ++sceneEditCalls_; }
    Q_INVOKABLE void selectScene(const QString&) {}
    Q_INVOKABLE void selectSource(const QString&) {}
    Q_INVOKABLE void toggleSource(const QString&) { ++sourceEditCalls_; }
    Q_INVOKABLE void moveSource(const QString&, int) { ++sourceEditCalls_; }
    Q_INVOKABLE void setSelectedTransform(double, double, double, double,
                                          double, double, double, double,
                                          double, double, double, double, int) {
        ++transformCalls_;
    }
    Q_INVOKABLE void resetSelectedTransform() { ++transformCalls_; }
    Q_INVOKABLE void applySelectedPipPreset(const QString&) { ++transformCalls_; }
    Q_INVOKABLE void retryReconciliation() {}
    void setBusy(bool busy) {
        if (busy_ == busy) return;
        busy_ = busy;
        emit stateChanged();
    }
    void setRecording(bool recording) {
        if (recording_ == recording) return;
        recording_ = recording;
        emit stateChanged();
    }
    void setActiveSceneId(QString sceneId) {
        activeSceneId_ = std::move(sceneId);
        emit stateChanged();
    }
    void setSelectedSourceId(QString sourceId) {
        selectedSourceId_ = std::move(sourceId);
        emit stateChanged();
        emit selectionChanged();
    }
    void setSceneCount(int count) { scenes_.setSceneCount(count); }
    [[nodiscard]] int switchCalls() const noexcept { return switchCalls_; }
    [[nodiscard]] int markerCalls() const noexcept { return markerCalls_; }
    [[nodiscard]] QString lastSceneId() const { return lastSceneId_; }
    [[nodiscard]] qlonglong lastPositionNs() const noexcept { return lastPositionNs_; }
    [[nodiscard]] int sceneEditCalls() const noexcept { return sceneEditCalls_; }
    [[nodiscard]] int sourceEditCalls() const noexcept { return sourceEditCalls_; }
    [[nodiscard]] int transformCalls() const noexcept { return transformCalls_; }

signals:
    void stateChanged();
    void selectionChanged();

private:
    FakeStudioSceneModel scenes_;
    FakeStudioSourceModel sources_;
    FakeStudioSourceModel activeSources_;
    bool busy_{false};
    bool recording_{true};
    QString activeSceneId_{QStringLiteral("scene-5")};
    QString selectedSourceId_{QStringLiteral("camera")};
    int switchCalls_{0};
    int markerCalls_{0};
    QString lastSceneId_;
    qlonglong lastPositionNs_{0};
    int sceneEditCalls_{0};
    int sourceEditCalls_{0};
    int transformCalls_{0};
};

class FakeScreenCaptureController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy CONSTANT)
    Q_PROPERTY(bool previewing READ previewing CONSTANT)
    Q_PROPERTY(bool canStopPreview READ canStopPreview CONSTANT)
    Q_PROPERTY(bool permissionRequired READ permissionRequired CONSTANT)
    Q_PROPERTY(QVariantList targets READ targets CONSTANT)
    Q_PROPERTY(QString selectedTargetId READ selectedTargetId CONSTANT)
    Q_PROPERTY(QString statusMessage READ statusMessage CONSTANT)
    Q_PROPERTY(quint32 actualWidth READ actualWidth CONSTANT)
    Q_PROPERTY(quint32 actualHeight READ actualHeight CONSTANT)
    Q_PROPERTY(qulonglong receivedFrames READ receivedFrames CONSTANT)
    Q_PROPERTY(qulonglong droppedFrames READ droppedFrames CONSTANT)
    Q_PROPERTY(qulonglong ignoredFrames READ ignoredFrames CONSTANT)
    Q_PROPERTY(qulonglong invalidFrames READ invalidFrames CONSTANT)
    Q_PROPERTY(qulonglong replacedPreviewFrames READ replacedPreviewFrames CONSTANT)
    Q_PROPERTY(double currentFps READ currentFps CONSTANT)

public:
    using QObject::QObject;
    [[nodiscard]] bool busy() const noexcept { return false; }
    [[nodiscard]] bool previewing() const noexcept { return false; }
    [[nodiscard]] bool canStopPreview() const noexcept { return false; }
    [[nodiscard]] bool permissionRequired() const noexcept { return false; }
    [[nodiscard]] QVariantList targets() const {
        return {QVariantMap{{QStringLiteral("id"), QStringLiteral("display:1")},
                            {QStringLiteral("kind"), QStringLiteral("display")},
                            {QStringLiteral("name"), QStringLiteral("Built-in Display")},
                            {QStringLiteral("application"), QString{}},
                            {QStringLiteral("width"), 1920},
                            {QStringLiteral("height"), 1080}}};
    }
    [[nodiscard]] QString selectedTargetId() const { return QStringLiteral("display:1"); }
    [[nodiscard]] QString statusMessage() const {
        return QStringLiteral("captured window closed");
    }
    [[nodiscard]] quint32 actualWidth() const noexcept { return 1920; }
    [[nodiscard]] quint32 actualHeight() const noexcept { return 1080; }
    [[nodiscard]] qulonglong receivedFrames() const noexcept { return 60; }
    [[nodiscard]] qulonglong droppedFrames() const noexcept { return 1; }
    [[nodiscard]] qulonglong ignoredFrames() const noexcept { return 2; }
    [[nodiscard]] qulonglong invalidFrames() const noexcept { return 3; }
    [[nodiscard]] qulonglong replacedPreviewFrames() const noexcept { return 2; }
    [[nodiscard]] double currentFps() const noexcept { return 59.94; }

    Q_INVOKABLE void initialize() {}
    Q_INVOKABLE void requestPermission() {}
    Q_INVOKABLE void refreshTargets() {}
    Q_INVOKABLE void selectTarget(const QString&) {}
    Q_INVOKABLE void startPreview() {}
    Q_INVOKABLE void stopPreview() {}
};

class FakeDeviceCaptureController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList cameras READ cameras CONSTANT)
    Q_PROPERTY(QVariantList microphones READ microphones CONSTANT)
    Q_PROPERTY(QString selectedCameraId READ selectedCameraId CONSTANT)
    Q_PROPERTY(QString selectedMicrophoneId READ selectedMicrophoneId CONSTANT)
    Q_PROPERTY(bool cameraPermissionRequired READ cameraPermissionRequired CONSTANT)
    Q_PROPERTY(bool microphonePermissionRequired READ microphonePermissionRequired CONSTANT)
    Q_PROPERTY(bool cameraCapturing READ cameraCapturing CONSTANT)
    Q_PROPERTY(bool microphoneCapturing READ microphoneCapturing CONSTANT)
    Q_PROPERTY(bool systemAudioCapturing READ systemAudioCapturing CONSTANT)
    Q_PROPERTY(bool cameraCanStop READ cameraCanStop CONSTANT)
    Q_PROPERTY(bool microphoneCanStop READ microphoneCanStop CONSTANT)
    Q_PROPERTY(bool systemAudioCanStop READ systemAudioCanStop CONSTANT)
    Q_PROPERTY(bool cameraBusy READ cameraBusy CONSTANT)
    Q_PROPERTY(bool microphoneBusy READ microphoneBusy CONSTANT)
    Q_PROPERTY(bool systemAudioBusy READ systemAudioBusy CONSTANT)
    Q_PROPERTY(QString cameraStatus READ cameraStatus CONSTANT)
    Q_PROPERTY(QString microphoneStatus READ microphoneStatus CONSTANT)
    Q_PROPERTY(QString systemAudioStatus READ systemAudioStatus CONSTANT)
    Q_PROPERTY(quint32 cameraWidth READ cameraWidth CONSTANT)
    Q_PROPERTY(quint32 cameraHeight READ cameraHeight CONSTANT)
    Q_PROPERTY(double cameraFps READ cameraFps CONSTANT)
    Q_PROPERTY(double microphonePeakDbfs READ microphonePeakDbfs CONSTANT)
    Q_PROPERTY(double microphoneRmsDbfs READ microphoneRmsDbfs CONSTANT)
    Q_PROPERTY(double systemAudioPeakDbfs READ systemAudioPeakDbfs CONSTANT)
    Q_PROPERTY(double systemAudioRmsDbfs READ systemAudioRmsDbfs CONSTANT)
    Q_PROPERTY(qulonglong microphoneBlocks READ microphoneBlocks CONSTANT)
    Q_PROPERTY(qulonglong systemAudioBlocks READ systemAudioBlocks CONSTANT)
    Q_PROPERTY(qulonglong microphoneOverruns READ microphoneOverruns CONSTANT)
    Q_PROPERTY(qulonglong systemAudioOverruns READ systemAudioOverruns CONSTANT)

public:
    using QObject::QObject;
    [[nodiscard]] QVariantList cameras() const {
        return {QVariantMap{{QStringLiteral("id"), QStringLiteral("camera:1")},
                            {QStringLiteral("name"), QStringLiteral("Studio Camera")},
                            {QStringLiteral("default"), true}}};
    }
    [[nodiscard]] QVariantList microphones() const {
        return {QVariantMap{{QStringLiteral("id"), QStringLiteral("mic:1")},
                            {QStringLiteral("name"), QStringLiteral("Studio Microphone")},
                            {QStringLiteral("default"), true}}};
    }
    [[nodiscard]] QString selectedCameraId() const { return QStringLiteral("camera:1"); }
    [[nodiscard]] QString selectedMicrophoneId() const { return QStringLiteral("mic:1"); }
    [[nodiscard]] bool cameraPermissionRequired() const noexcept { return false; }
    [[nodiscard]] bool microphonePermissionRequired() const noexcept { return false; }
    [[nodiscard]] bool cameraCapturing() const noexcept { return true; }
    [[nodiscard]] bool microphoneCapturing() const noexcept { return true; }
    [[nodiscard]] bool systemAudioCapturing() const noexcept { return true; }
    [[nodiscard]] bool cameraCanStop() const noexcept { return true; }
    [[nodiscard]] bool microphoneCanStop() const noexcept { return true; }
    [[nodiscard]] bool systemAudioCanStop() const noexcept { return true; }
    [[nodiscard]] bool cameraBusy() const noexcept { return false; }
    [[nodiscard]] bool microphoneBusy() const noexcept { return false; }
    [[nodiscard]] bool systemAudioBusy() const noexcept { return false; }
    [[nodiscard]] QString cameraStatus() const { return QStringLiteral("Camera capturing"); }
    [[nodiscard]] QString microphoneStatus() const {
        return QStringLiteral("Microphone capturing");
    }
    [[nodiscard]] QString systemAudioStatus() const {
        return QStringLiteral("System audio capturing");
    }
    [[nodiscard]] quint32 cameraWidth() const noexcept { return 1920; }
    [[nodiscard]] quint32 cameraHeight() const noexcept { return 1080; }
    [[nodiscard]] double cameraFps() const noexcept { return 30.0; }
    [[nodiscard]] double microphonePeakDbfs() const noexcept { return -6.0; }
    [[nodiscard]] double microphoneRmsDbfs() const noexcept { return -12.0; }
    [[nodiscard]] double systemAudioPeakDbfs() const noexcept { return -9.0; }
    [[nodiscard]] double systemAudioRmsDbfs() const noexcept { return -18.0; }
    [[nodiscard]] qulonglong microphoneBlocks() const noexcept { return 42; }
    [[nodiscard]] qulonglong systemAudioBlocks() const noexcept { return 43; }
    [[nodiscard]] qulonglong microphoneOverruns() const noexcept { return 0; }
    [[nodiscard]] qulonglong systemAudioOverruns() const noexcept { return 1; }

    Q_INVOKABLE void initialize() {}
    Q_INVOKABLE void refreshDevices() {}
    Q_INVOKABLE void requestCameraPermission() {}
    Q_INVOKABLE void requestMicrophonePermission() {}
    Q_INVOKABLE void selectCamera(const QString&) {}
    Q_INVOKABLE void selectMicrophone(const QString&) {}
    Q_INVOKABLE void setCameraEnabled(bool) {}
    Q_INVOKABLE void setMicrophoneEnabled(bool) {}
    Q_INVOKABLE void setSystemAudioEnabled(bool) {}
};

class FakeEditorController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* mediaBinModel READ mediaBinModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* timelineTrackModel READ timelineTrackModel CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY editStateChanged)
    Q_PROPERTY(bool previewStale READ previewStale CONSTANT)
    Q_PROPERTY(bool playing READ playing CONSTANT)
    Q_PROPERTY(qlonglong playheadNs READ playheadNs CONSTANT)
    Q_PROPERTY(qlonglong timelineDurationNs READ timelineDurationNs CONSTANT)
    Q_PROPERTY(qlonglong timelineRevision READ timelineRevision CONSTANT)
    Q_PROPERTY(QString statusMessage READ statusMessage CONSTANT)
    Q_PROPERTY(QImage previewImage READ previewImage CONSTANT)
    Q_PROPERTY(bool hasPreviewFrame READ hasPreviewFrame CONSTANT)
    Q_PROPERTY(bool sessionBusy READ sessionBusy NOTIFY editStateChanged)
    Q_PROPERTY(QString selectedTrackId READ selectedTrackId NOTIFY editStateChanged)
    Q_PROPERTY(QString selectedClipId READ selectedClipId NOTIFY editStateChanged)
    Q_PROPERTY(QString selectedClipKind READ selectedClipKind NOTIFY editStateChanged)
    Q_PROPERTY(bool selectedVisualCompatible READ selectedVisualCompatible NOTIFY editStateChanged)
    Q_PROPERTY(bool selectedAudioCompatible READ selectedAudioCompatible NOTIFY editStateChanged)
    Q_PROPERTY(QVariantMap selectedVisualTransform READ selectedVisualTransform NOTIFY editStateChanged)
    Q_PROPERTY(QVariantMap selectedAudioEnvelope READ selectedAudioEnvelope NOTIFY editStateChanged)
    Q_PROPERTY(QVariantMap selectedTitlePayload READ selectedTitlePayload NOTIFY editStateChanged)
    Q_PROPERTY(QVariantList selectedCaptionCues READ selectedCaptionCues NOTIFY editStateChanged)
    Q_PROPERTY(QString selectedPipPreset READ selectedPipPreset NOTIFY editStateChanged)
    Q_PROPERTY(QString selectedResolvedFontFamily READ selectedResolvedFontFamily NOTIFY editStateChanged)
    Q_PROPERTY(qlonglong selectedClipStartNs READ selectedClipStartNs NOTIFY editStateChanged)
    Q_PROPERTY(qlonglong selectedClipEndNs READ selectedClipEndNs NOTIFY editStateChanged)
    Q_PROPERTY(qlonglong rangeInNs READ rangeInNs CONSTANT)
    Q_PROPERTY(qlonglong rangeOutNs READ rangeOutNs CONSTANT)
    Q_PROPERTY(bool hasMarkedRange READ hasMarkedRange CONSTANT)
    Q_PROPERTY(bool canUndo READ canUndo CONSTANT)
    Q_PROPERTY(bool canRedo READ canRedo CONSTANT)
    Q_PROPERTY(bool clean READ clean CONSTANT)

public:
    explicit FakeEditorController(QObject* parent = nullptr) : QObject(parent) {
        using namespace creator;
        const auto media = domain::MediaAsset::create(
                               domain::AssetId::create("screen").value(),
                               domain::MediaKind::Video,
                               "media/화면 녹화.mp4", core::DurationNs{5'000'000'000},
                               domain::VideoAssetMetadata{
                                   1920, 1080, core::FrameRate::create(60, 1).value()},
                               std::nullopt, 42'000, "fingerprint",
                               domain::AssetAvailability::Offline)
                               .value();
        mediaBin_.setAssets({media});
        auto timeline =
            domain::Timeline::create(domain::TimelineId::create("main").value(),
                                     "강의 편집",
                                     core::FrameRate::create(60, 1).value())
                .value();
        static_cast<void>(timeline.addTrack(
            domain::Track::create(domain::TrackId::create("v1").value(),
                                  domain::TrackKind::Video, "화면", true, false)
                .value()));
        const auto source = domain::TimeRange::create(
                                core::TimestampNs{core::DurationNs::zero()},
                                core::DurationNs{2'000'000'000})
                                .value();
        const auto placed = domain::TimeRange::create(
                                core::TimestampNs{core::DurationNs{1'000'000'000}},
                                core::DurationNs{2'000'000'000})
                                .value();
        static_cast<void>(timeline.insertClip(
            domain::TrackId::create("v1").value(),
            domain::Clip::createAsset(domain::ClipId::create("clip-1").value(),
                                      media, source, placed, true, std::nullopt,
                                      std::nullopt)
                .value()));
        static_cast<void>(timeline.addTrack(
            domain::Track::create(domain::TrackId::create("title-1").value(),
                                  domain::TrackKind::Title, "Titles", true,
                                  false)
                .value()));
        const auto title = domain::TitlePayload::create(
                               "강의 제목", "Noto Sans", 0.2, 0.1,
                               domain::RgbaColor::parse("#ffffffff").value(),
                               domain::RgbaColor::parse("#00000080").value(),
                               domain::TextAlignment::Center)
                               .value();
        static_cast<void>(timeline.insertClip(
            domain::TrackId::create("title-1").value(),
            domain::Clip::createTitle(
                domain::ClipId::create("title-ui").value(), placed, true,
                title, std::nullopt)
                .value()));
        static_cast<void>(timeline.addTrack(
            domain::Track::create(
                domain::TrackId::create("caption-1").value(),
                domain::TrackKind::Caption, "Captions", true, false)
                .value()));
        const auto cue = domain::CaptionCue::create(
                             domain::CueId::create("cue-ui").value(),
                             core::DurationNs{100'000'000},
                             core::DurationNs{500'000'000}, "첫 자막")
                             .value();
        static_cast<void>(timeline.insertClip(
            domain::TrackId::create("caption-1").value(),
            domain::Clip::createCaption(
                domain::ClipId::create("caption-ui").value(), placed, true,
                {cue}, std::nullopt)
                .value()));
        tracks_.setTimeline(std::move(timeline));
    }

    [[nodiscard]] QAbstractItemModel* mediaBinModel() noexcept {
        return &mediaBin_;
    }
    [[nodiscard]] QAbstractItemModel* timelineTrackModel() noexcept {
        return &tracks_;
    }
    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] bool previewStale() const noexcept { return true; }
    [[nodiscard]] bool playing() const noexcept { return false; }
    [[nodiscard]] qlonglong playheadNs() const noexcept { return 1'000'000'000; }
    [[nodiscard]] qlonglong timelineDurationNs() const noexcept {
        return 3'000'000'000;
    }
    [[nodiscard]] qlonglong lastSeekNs() const noexcept { return lastSeekNs_; }
    [[nodiscard]] qlonglong timelineRevision() const noexcept { return 4; }
    [[nodiscard]] QString statusMessage() const {
        return QStringLiteral("Preview engine unavailable");
    }
    [[nodiscard]] QImage previewImage() const {
        QImage image{2, 1, QImage::Format_ARGB32};
        image.fill(QColor(30, 60, 90));
        return image;
    }
    [[nodiscard]] bool hasPreviewFrame() const noexcept { return true; }
    [[nodiscard]] bool sessionBusy() const noexcept { return sessionBusy_; }
    [[nodiscard]] QString selectedTrackId() const { return selectedTrackId_; }
    [[nodiscard]] QString selectedClipId() const { return selectedClipId_; }
    [[nodiscard]] QString selectedClipKind() const {
        if (selectedClipId_.isEmpty()) return {};
        if (selectedTrackId_ == QStringLiteral("title-1")) {
            return QStringLiteral("title");
        }
        if (selectedTrackId_ == QStringLiteral("caption-1")) {
            return QStringLiteral("caption");
        }
        return QStringLiteral("asset");
    }
    [[nodiscard]] bool selectedVisualCompatible() const noexcept {
        return !selectedClipId_.isEmpty()
               && selectedTrackId_ != QStringLiteral("a1");
    }
    [[nodiscard]] bool selectedAudioCompatible() const noexcept {
        return selectedClipKind() == QStringLiteral("asset");
    }
    [[nodiscard]] QVariantMap selectedVisualTransform() const {
        return {{QStringLiteral("x"), 0.1},
                {QStringLiteral("y"), 0.2},
                {QStringLiteral("width"), 0.3},
                {QStringLiteral("height"), 0.4},
                {QStringLiteral("scaleX"), 1.0},
                {QStringLiteral("scaleY"), 1.0},
                {QStringLiteral("rotationDegrees"), 5.0},
                {QStringLiteral("cropLeft"), 0.0},
                {QStringLiteral("cropTop"), 0.0},
                {QStringLiteral("cropRight"), 0.0},
                {QStringLiteral("cropBottom"), 0.0},
                {QStringLiteral("opacity"), 0.8},
                {QStringLiteral("zOrder"), 2}};
    }
    [[nodiscard]] QVariantMap selectedAudioEnvelope() const {
        return {{QStringLiteral("gainDb"), -3.0},
                {QStringLiteral("fadeInNs"), 100'000'000LL},
                {QStringLiteral("fadeOutNs"), 200'000'000LL}};
    }
    [[nodiscard]] QVariantMap selectedTitlePayload() const {
        if (selectedClipKind() != QStringLiteral("title")) return {};
        return {{QStringLiteral("text"), QString::fromUtf8("강의 제목")},
                {QStringLiteral("fontFamily"), QStringLiteral("Noto Sans")},
                {QStringLiteral("x"), 0.2},
                {QStringLiteral("y"), 0.1},
                {QStringLiteral("foreground"), QStringLiteral("#ffffffff")},
                {QStringLiteral("background"), QStringLiteral("#00000080")},
                {QStringLiteral("alignment"), QStringLiteral("center")}};
    }
    [[nodiscard]] QVariantList selectedCaptionCues() const {
        if (selectedClipKind() != QStringLiteral("caption")) return {};
        return {QVariantMap{{QStringLiteral("cueId"), QStringLiteral("cue-ui")},
                            {QStringLiteral("startOffsetNs"), 100'000'000LL},
                            {QStringLiteral("durationNs"), 500'000'000LL},
                            {QStringLiteral("text"),
                             QString::fromUtf8("첫 자막")}}};
    }
    [[nodiscard]] QString selectedPipPreset() const {
        return QStringLiteral("custom");
    }
    [[nodiscard]] QString selectedResolvedFontFamily() const {
        return selectedClipKind() == QStringLiteral("title")
                   ? QStringLiteral("Noto Sans CJK KR")
                   : QString{};
    }
    [[nodiscard]] qlonglong selectedClipStartNs() const noexcept {
        return selectedClipId_.isEmpty() ? -1 : 1'000'000'000;
    }
    [[nodiscard]] qlonglong selectedClipEndNs() const noexcept {
        return selectedClipId_.isEmpty() ? -1 : 3'000'000'000;
    }
    [[nodiscard]] qlonglong rangeInNs() const noexcept { return 1'100'000'000; }
    [[nodiscard]] qlonglong rangeOutNs() const noexcept { return 1'900'000'000; }
    [[nodiscard]] bool hasMarkedRange() const noexcept { return true; }
    [[nodiscard]] bool canUndo() const noexcept { return true; }
    [[nodiscard]] bool canRedo() const noexcept { return true; }
    [[nodiscard]] bool clean() const noexcept { return false; }
    Q_INVOKABLE void play() {}
    Q_INVOKABLE void pause() {}
    Q_INVOKABLE void seek(qlonglong position) { lastSeekNs_ = position; }
    Q_INVOKABLE void selectClip(const QString& trackId, const QString& clipId) {
        selectedTrackId_ = trackId;
        selectedClipId_ = clipId;
        ++selectCalls_;
        emit editStateChanged();
        emit selectionChanged();
    }
    void clearSelection() {
        selectedTrackId_.clear();
        selectedClipId_.clear();
        emit editStateChanged();
        emit selectionChanged();
    }
    void setBusy(bool busy) {
        busy_ = busy;
        emit editStateChanged();
    }
    void setSessionBusy(bool busy) {
        sessionBusy_ = busy;
        emit editStateChanged();
    }
    Q_INVOKABLE void splitSelected() { ++splitCalls_; }
    Q_INVOKABLE void trimSelectedStart() { ++trimStartCalls_; }
    Q_INVOKABLE void trimSelectedEnd() { ++trimEndCalls_; }
    Q_INVOKABLE void markRangeIn() { ++markInCalls_; }
    Q_INVOKABLE void markRangeOut() { ++markOutCalls_; }
    Q_INVOKABLE void deleteMarkedRange(bool ripple) {
        ripple ? ++rippleCalls_ : ++liftCalls_;
    }
    Q_INVOKABLE void undo() { ++undoCalls_; }
    Q_INVOKABLE void redo() { ++redoCalls_; }
    Q_INVOKABLE void save() { ++saveCalls_; }
    Q_INVOKABLE void applySelectedVisualTransform(
        double, double, double, double, double, double, double, double, double,
        double, double, double, int) {
        ++visualApplyCalls_;
    }
    Q_INVOKABLE void applySelectedPipPreset(const QString&) {
        ++pipPresetCalls_;
    }
    Q_INVOKABLE void resetSelectedVisualTransform() { ++visualResetCalls_; }
    Q_INVOKABLE void applySelectedAudioEnvelope(double, qlonglong, qlonglong) {
        ++audioApplyCalls_;
    }
    Q_INVOKABLE void resetSelectedAudioEnvelope() { ++audioResetCalls_; }
    Q_INVOKABLE void addTitle(const QString&, const QString&, double, double,
                              const QString&, const QString&, const QString&) {
        ++titleAddCalls_;
    }
    Q_INVOKABLE void editSelectedTitle(
        const QString&, const QString&, double, double, const QString&,
        const QString&, const QString&) {
        ++titleEditCalls_;
    }
    Q_INVOKABLE void removeSelectedTitle() { ++titleRemoveCalls_; }
    Q_INVOKABLE void addCaptionCue(qlonglong, qlonglong, const QString&) {
        ++captionAddCalls_;
    }
    Q_INVOKABLE void editCaptionCue(const QString&, qlonglong, qlonglong,
                                    const QString&) {
        ++captionEditCalls_;
    }
    Q_INVOKABLE void removeCaptionCue(const QString&) {
        ++captionRemoveCalls_;
    }

    [[nodiscard]] int selectCalls() const noexcept { return selectCalls_; }
    [[nodiscard]] int splitCalls() const noexcept { return splitCalls_; }
    [[nodiscard]] int trimStartCalls() const noexcept { return trimStartCalls_; }
    [[nodiscard]] int trimEndCalls() const noexcept { return trimEndCalls_; }
    [[nodiscard]] int markInCalls() const noexcept { return markInCalls_; }
    [[nodiscard]] int markOutCalls() const noexcept { return markOutCalls_; }
    [[nodiscard]] int liftCalls() const noexcept { return liftCalls_; }
    [[nodiscard]] int rippleCalls() const noexcept { return rippleCalls_; }
    [[nodiscard]] int undoCalls() const noexcept { return undoCalls_; }
    [[nodiscard]] int redoCalls() const noexcept { return redoCalls_; }
    [[nodiscard]] int saveCalls() const noexcept { return saveCalls_; }
    [[nodiscard]] int visualApplyCalls() const noexcept {
        return visualApplyCalls_;
    }
    [[nodiscard]] int pipPresetCalls() const noexcept { return pipPresetCalls_; }
    [[nodiscard]] int audioApplyCalls() const noexcept {
        return audioApplyCalls_;
    }
    [[nodiscard]] int titleAddCalls() const noexcept { return titleAddCalls_; }
    [[nodiscard]] int titleEditCalls() const noexcept { return titleEditCalls_; }
    [[nodiscard]] int titleRemoveCalls() const noexcept {
        return titleRemoveCalls_;
    }
    [[nodiscard]] int captionAddCalls() const noexcept {
        return captionAddCalls_;
    }
    [[nodiscard]] int captionEditCalls() const noexcept {
        return captionEditCalls_;
    }
    [[nodiscard]] int captionRemoveCalls() const noexcept {
        return captionRemoveCalls_;
    }

signals:
    void editStateChanged();
    void selectionChanged();

private:
    creator::app::MediaBinModel mediaBin_;
    creator::app::TimelineTrackModel tracks_;
    qlonglong lastSeekNs_{-1};
    QString selectedTrackId_;
    QString selectedClipId_;
    bool busy_{false};
    bool sessionBusy_{false};
    int selectCalls_{0};
    int splitCalls_{0};
    int trimStartCalls_{0};
    int trimEndCalls_{0};
    int markInCalls_{0};
    int markOutCalls_{0};
    int liftCalls_{0};
    int rippleCalls_{0};
    int undoCalls_{0};
    int redoCalls_{0};
    int saveCalls_{0};
    int visualApplyCalls_{0};
    int pipPresetCalls_{0};
    int visualResetCalls_{0};
    int audioApplyCalls_{0};
    int audioResetCalls_{0};
    int titleAddCalls_{0};
    int titleEditCalls_{0};
    int titleRemoveCalls_{0};
    int captionAddCalls_{0};
    int captionEditCalls_{0};
    int captionRemoveCalls_{0};
};

TEST(QmlSmokeTest, RecoveryPageLoadsWithProjectControllerContract) {
    QQmlEngine engine;
    FakeProjectController controller;
    engine.rootContext()->setContextProperty(QStringLiteral("projectController"), &controller);
    QQmlComponent component{
        &engine,
        QUrl::fromLocalFile(QString::fromUtf8(CS_QML_SOURCE_DIR "/RecoveryPage.qml"))};

    std::unique_ptr<QObject> object{component.create()};

    ASSERT_NE(object, nullptr) << component.errorString().toStdString();
}

TEST(QmlSmokeTest, MainOpensRecoveryWhenStartupScanAlreadyFinished) {
    QQmlEngine engine;
    FakeProjectController projectController;
    FakeStudioController studioController;
    FakeScreenCaptureController screenCaptureController;
    FakeDeviceCaptureController deviceCaptureController;
    FakeEditorController editorController;
    FakeStudioWorkflowController studioWorkflowController;
    FakeShortcutSettingsController shortcutSettingsController;
    FakeExportController exportController;
    engine.rootContext()->setContextProperty(QStringLiteral("projectController"),
                                             &projectController);
    engine.rootContext()->setContextProperty(QStringLiteral("studioController"),
                                             &studioController);
    engine.rootContext()->setContextProperty(QStringLiteral("screenCaptureController"),
                                             &screenCaptureController);
    engine.rootContext()->setContextProperty(QStringLiteral("deviceCaptureController"),
                                             &deviceCaptureController);
    engine.rootContext()->setContextProperty(QStringLiteral("editorController"),
                                             &editorController);
    engine.rootContext()->setContextProperty(
        QStringLiteral("studioWorkflowController"), &studioWorkflowController);
    engine.rootContext()->setContextProperty(
        QStringLiteral("shortcutSettingsController"), &shortcutSettingsController);
    engine.rootContext()->setContextProperty(QStringLiteral("exportController"),
                                             &exportController);
    QQmlComponent component{
        &engine, QUrl::fromLocalFile(QString::fromUtf8(CS_QML_SOURCE_DIR "/Main.qml"))};

    std::unique_ptr<QObject> object{component.create()};

    ASSERT_NE(object, nullptr) << component.errorString().toStdString();
    EXPECT_EQ(object->property("currentPage").toString(), QStringLiteral("Recovery"));
}

TEST(QmlSmokeTest, MainRecordShortcutSharesVisibleActionAndStateGuard) {
    QQmlEngine engine;
    FakeProjectController projectController;
    FakeStudioController studioController;
    FakeScreenCaptureController screenCaptureController;
    FakeDeviceCaptureController deviceCaptureController;
    FakeEditorController editorController;
    FakeStudioWorkflowController studioWorkflowController;
    FakeShortcutSettingsController shortcutSettingsController;
    FakeExportController exportController;
    projectController.setHasOpenProject(true);
    engine.rootContext()->setContextProperty(QStringLiteral("projectController"),
                                             &projectController);
    engine.rootContext()->setContextProperty(QStringLiteral("studioController"),
                                             &studioController);
    engine.rootContext()->setContextProperty(QStringLiteral("screenCaptureController"),
                                             &screenCaptureController);
    engine.rootContext()->setContextProperty(QStringLiteral("deviceCaptureController"),
                                             &deviceCaptureController);
    engine.rootContext()->setContextProperty(QStringLiteral("editorController"),
                                             &editorController);
    engine.rootContext()->setContextProperty(
        QStringLiteral("studioWorkflowController"), &studioWorkflowController);
    engine.rootContext()->setContextProperty(
        QStringLiteral("shortcutSettingsController"), &shortcutSettingsController);
    engine.rootContext()->setContextProperty(QStringLiteral("exportController"),
                                             &exportController);
    QQmlComponent component{
        &engine, QUrl::fromLocalFile(QString::fromUtf8(CS_QML_SOURCE_DIR "/Main.qml"))};
    std::unique_ptr<QObject> object{component.create()};
    ASSERT_NE(object, nullptr) << component.errorString().toStdString();
    object->setProperty("currentPage", QStringLiteral("Studio"));
    QCoreApplication::processEvents();

    auto* action = object->findChild<QObject*>(QStringLiteral("studioRecordAction"));
    auto* button = object->findChild<QObject*>(QStringLiteral("studioRecordButton"));
    auto* shortcut = object->findChild<QObject*>(QStringLiteral("studioRecordShortcut"));
    ASSERT_NE(action, nullptr);
    ASSERT_NE(button, nullptr);
    ASSERT_NE(shortcut, nullptr);
    EXPECT_EQ(button->property("action").value<QObject*>(), action);
    EXPECT_TRUE(action->property("enabled").toBool());
    EXPECT_TRUE(shortcut->property("enabled").toBool());

    ASSERT_TRUE(QMetaObject::invokeMethod(action, "trigger"));
    EXPECT_EQ(studioController.startCalls(), 1);
    studioController.setRecording(false);
    ASSERT_TRUE(QMetaObject::invokeMethod(shortcut, "activated"));
    EXPECT_EQ(studioController.startCalls(), 2);

    studioController.setBusy(true);
    QCoreApplication::processEvents();
    EXPECT_FALSE(action->property("enabled").toBool());
    EXPECT_FALSE(shortcut->property("enabled").toBool());
}

TEST(QmlSmokeTest, ExportPageExposesProductPresetsProgressAndCancellation) {
    QQmlEngine engine;
    FakeExportController controller;
    QQmlComponent component{
        &engine,
        QUrl::fromLocalFile(QString::fromUtf8(CS_QML_SOURCE_DIR "/ExportPage.qml"))};
    QVariantMap initialProperties{
        {QStringLiteral("controller"),
         QVariant::fromValue(static_cast<QObject*>(&controller))},
        {QStringLiteral("width"), 900},
        {QStringLiteral("height"), 700}};
    std::unique_ptr<QObject> object{
        component.createWithInitialProperties(initialProperties)};

    ASSERT_NE(object, nullptr) << component.errorString().toStdString();
    auto* preset = object->findChild<QObject*>(QStringLiteral("exportPreset"));
    auto* start =
        object->findChild<QObject*>(QStringLiteral("exportStartButton"));
    auto* cancel =
        object->findChild<QObject*>(QStringLiteral("exportCancelButton"));
    auto* progress =
        object->findChild<QObject*>(QStringLiteral("exportProgress"));
    ASSERT_NE(preset, nullptr);
    ASSERT_NE(start, nullptr);
    ASSERT_NE(cancel, nullptr);
    ASSERT_NE(progress, nullptr);
    EXPECT_EQ(preset->property("count").toInt(), 2);
    EXPECT_TRUE(start->property("enabled").toBool());
    EXPECT_FALSE(cancel->property("visible").toBool());

    controller.setBusy(true);
    QCoreApplication::processEvents();
    EXPECT_FALSE(start->property("enabled").toBool());
    EXPECT_TRUE(cancel->property("visible").toBool());
    EXPECT_TRUE(cancel->property("enabled").toBool());
    EXPECT_DOUBLE_EQ(progress->property("value").toDouble(), 0.4);
    ASSERT_TRUE(QMetaObject::invokeMethod(cancel, "clicked"));
    EXPECT_EQ(controller.cancelCalls(), 1);

    controller.setBusy(false);
    controller.setMaximumExportHeight(1'080);
    QCoreApplication::processEvents();
    EXPECT_EQ(preset->property("count").toInt(), 1);
    controller.setExportAllowed(false);
    QCoreApplication::processEvents();
    EXPECT_FALSE(start->property("enabled").toBool());
}

TEST(QmlSmokeTest, EditorPageRendersTypedModelsAndPreviewState) {
    QQmlEngine engine;
    FakeEditorController controller;
    QQmlComponent component{
        &engine,
        QUrl::fromLocalFile(QString::fromUtf8(CS_QML_SOURCE_DIR "/EditorPage.qml"))};
    QVariantMap initialProperties{
        {QStringLiteral("controller"),
         QVariant::fromValue(static_cast<QObject*>(&controller))},
        {QStringLiteral("width"), 1200},
        {QStringLiteral("height"), 720}};

    QQuickWindow window;
    window.setGeometry(0, 0, 1200, 720);
    std::unique_ptr<QObject> object{
        component.createWithInitialProperties(initialProperties)};

    ASSERT_NE(object, nullptr) << component.errorString().toStdString();
    auto* rootItem = qobject_cast<QQuickItem*>(object.get());
    ASSERT_NE(rootItem, nullptr);
    rootItem->setParentItem(window.contentItem());
    window.show();
    for (int attempt = 0; attempt < 20; ++attempt) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    auto* media = findVisualItem(rootItem, QStringLiteral("mediaAsset-screen"));
    auto* mediaList =
        object->findChild<QObject*>(QStringLiteral("editorMediaList"));
    auto* offline = findVisualItem(rootItem, QStringLiteral("mediaOffline-screen"));
    auto* track = findVisualItem(rootItem, QStringLiteral("timelineTrack-v1"));
    auto* clip = findVisualItem(rootItem, QStringLiteral("timelineClip-clip-1"));
    auto* preview = object->findChild<QObject*>(QStringLiteral("editorPreviewState"));
    auto* previewSurface =
        object->findChild<QObject*>(QStringLiteral("editorPreviewSurface"));
    auto* status = object->findChild<QObject*>(QStringLiteral("editorStatus"));
    auto* seekSlider =
        object->findChild<QObject*>(QStringLiteral("editorSeekSlider"));
    ASSERT_NE(mediaList, nullptr);
    ASSERT_EQ(mediaList->property("count").toInt(), 1);
    ASSERT_NE(media, nullptr);
    ASSERT_NE(offline, nullptr);
    ASSERT_NE(track, nullptr);
    ASSERT_NE(clip, nullptr);
    ASSERT_NE(preview, nullptr);
    ASSERT_NE(previewSurface, nullptr);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(seekSlider, nullptr);
    EXPECT_TRUE(media->property("text").toString().contains(
        QString::fromUtf8("화면 녹화.mp4")));
    EXPECT_TRUE(offline->property("visible").toBool());
    EXPECT_EQ(track->property("text").toString(), QString::fromUtf8("화면"));
    EXPECT_GT(clip->property("width").toDouble(), 0.0);
    EXPECT_TRUE(preview->property("text").toString().contains(
        QStringLiteral("stale"), Qt::CaseInsensitive));
    EXPECT_TRUE(previewSurface->property("hasFrame").toBool());
    EXPECT_TRUE(previewSurface->property("stale").toBool());
    EXPECT_EQ(status->property("text").toString(),
              QStringLiteral("Preview engine unavailable"));
    EXPECT_EQ(seekSlider->property("to").toLongLong(), 3'000'000'000);
    seekSlider->setProperty("value", 1'500'000'000);
    ASSERT_TRUE(QMetaObject::invokeMethod(seekSlider, "moved"));
    EXPECT_EQ(controller.lastSeekNs(), 1'500'000'000);
}

TEST(QmlSmokeTest, EditorPageExposesCompactTouchWorkspaceAtPhoneSize) {
    QQmlEngine engine;
    FakeEditorController controller;
    QSignalSpy warningSpy{&engine, &QQmlEngine::warnings};
    QQmlComponent component{
        &engine,
        QUrl::fromLocalFile(QString::fromUtf8(CS_QML_SOURCE_DIR "/EditorPage.qml"))};
    QVariantMap initialProperties{
        {QStringLiteral("controller"),
         QVariant::fromValue(static_cast<QObject*>(&controller))},
        {QStringLiteral("width"), 360},
        {QStringLiteral("height"), 640}};
    std::unique_ptr<QObject> object{
        component.createWithInitialProperties(initialProperties)};

    ASSERT_NE(object, nullptr) << component.errorString().toStdString();
    auto* rootItem = qobject_cast<QQuickItem*>(object.get());
    ASSERT_NE(rootItem, nullptr);
    QQuickWindow window;
    window.setGeometry(0, 0, 360, 640);
    rootItem->setParentItem(window.contentItem());
    window.show();
    QCoreApplication::processEvents();

    auto* tabs = object->findChild<QObject*>(QStringLiteral("editorCompactTabs"));
    auto* preview = object->findChild<QObject*>(QStringLiteral("editorPreviewPane"));
    auto* media = object->findChild<QObject*>(QStringLiteral("editorMediaPane"));
    auto* inspector = object->findChild<QObject*>(QStringLiteral("editorInspectorPane"));
    ASSERT_NE(tabs, nullptr);
    ASSERT_NE(preview, nullptr);
    ASSERT_NE(media, nullptr);
    ASSERT_NE(inspector, nullptr);
    EXPECT_TRUE(tabs->property("visible").toBool());
    EXPECT_TRUE(preview->property("visible").toBool());
    EXPECT_FALSE(media->property("visible").toBool());

    rootItem->setProperty("compactSection", QStringLiteral("inspector"));
    QCoreApplication::processEvents();
    EXPECT_FALSE(preview->property("visible").toBool());
    EXPECT_TRUE(inspector->property("visible").toBool());
    EXPECT_EQ(warningSpy.count(), 0);
}

TEST(QmlSmokeTest, EditorPageProvidesDurableEditControls) {
    QQmlEngine engine;
    FakeEditorController controller;
    QQmlComponent component{
        &engine,
        QUrl::fromLocalFile(QString::fromUtf8(CS_QML_SOURCE_DIR "/EditorPage.qml"))};
    QVariantMap initialProperties{
        {QStringLiteral("controller"),
         QVariant::fromValue(static_cast<QObject*>(&controller))},
        {QStringLiteral("width"), 1200},
        {QStringLiteral("height"), 720}};
    QQuickWindow window;
    window.setGeometry(0, 0, 1200, 720);
    std::unique_ptr<QObject> object{
        component.createWithInitialProperties(initialProperties)};
    ASSERT_NE(object, nullptr) << component.errorString().toStdString();
    auto* rootItem = qobject_cast<QQuickItem*>(object.get());
    ASSERT_NE(rootItem, nullptr);
    rootItem->setParentItem(window.contentItem());
    window.show();
    for (int attempt = 0; attempt < 20; ++attempt) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }

    const QStringList buttons{
        QStringLiteral("editorSplitButton"),
        QStringLiteral("editorTrimStartButton"),
        QStringLiteral("editorTrimEndButton"),
        QStringLiteral("editorUndoButton"),
        QStringLiteral("editorRedoButton"),
        QStringLiteral("editorSaveButton"),
        QStringLiteral("editorMarkInButton"),
        QStringLiteral("editorMarkOutButton"),
        QStringLiteral("editorLiftButton"),
        QStringLiteral("editorRippleDeleteButton"),
    };
    for (const auto& name : buttons) {
        auto* button = object->findChild<QObject*>(name);
        ASSERT_NE(button, nullptr) << name.toStdString();
        ASSERT_TRUE(QMetaObject::invokeMethod(button, "clicked"));
    }

    auto* clip = findVisualItem(rootItem, QStringLiteral("timelineClip-clip-1"));
    ASSERT_NE(clip, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(clip, "activateSelection"));
    EXPECT_EQ(controller.selectCalls(), 1);
    EXPECT_EQ(controller.selectedTrackId(), QStringLiteral("v1"));
    EXPECT_EQ(controller.selectedClipId(), QStringLiteral("clip-1"));

    const QStringList shortcuts{
        QStringLiteral("editorSplitShortcut"),
        QStringLiteral("editorMarkInShortcut"),
        QStringLiteral("editorMarkOutShortcut"),
        QStringLiteral("editorLiftShortcut"),
        QStringLiteral("editorRippleDeleteShortcut"),
        QStringLiteral("editorUndoShortcut"),
        QStringLiteral("editorRedoShortcut"),
        QStringLiteral("editorSaveShortcut"),
    };
    for (const auto& name : shortcuts) {
        auto* shortcut = object->findChild<QObject*>(name);
        ASSERT_NE(shortcut, nullptr) << name.toStdString();
        ASSERT_TRUE(QMetaObject::invokeMethod(shortcut, "activated"));
    }

    EXPECT_EQ(controller.splitCalls(), 2);
    EXPECT_EQ(controller.trimStartCalls(), 1);
    EXPECT_EQ(controller.trimEndCalls(), 1);
    EXPECT_EQ(controller.markInCalls(), 2);
    EXPECT_EQ(controller.markOutCalls(), 2);
    EXPECT_EQ(controller.liftCalls(), 2);
    EXPECT_EQ(controller.rippleCalls(), 2);
    EXPECT_EQ(controller.undoCalls(), 2);
    EXPECT_EQ(controller.redoCalls(), 2);
    EXPECT_EQ(controller.saveCalls(), 2);
    ASSERT_NE(object->findChild<QObject*>(QStringLiteral("editorMarkedRangeLabel")),
              nullptr);
    ASSERT_NE(object->findChild<QObject*>(QStringLiteral("editorSelectionLabel")),
              nullptr);
    auto* bounds = object->findChild<QObject*>(
        QStringLiteral("editorSelectedClipBoundsLabel"));
    ASSERT_NE(bounds, nullptr);
    EXPECT_TRUE(bounds->property("text").toString().contains(
        QStringLiteral("1.00 s")));
    EXPECT_TRUE(bounds->property("text").toString().contains(
        QStringLiteral("3.00 s")));
    auto* range = object->findChild<QObject*>(
        QStringLiteral("editorMarkedRangeLabel"));
    ASSERT_NE(range, nullptr);
    EXPECT_TRUE(range->property("text").toString().contains(
        QString::fromUtf8("→")));
    EXPECT_FALSE(range->property("text").toString().contains(
        QStringLiteral("??")));
}

TEST(QmlSmokeTest, EditorPageProvidesCompleteAccessibleInspectorControls) {
    QQmlEngine engine;
    FakeEditorController controller;
    QQmlComponent component{
        &engine,
        QUrl::fromLocalFile(QString::fromUtf8(CS_QML_SOURCE_DIR "/EditorPage.qml"))};
    QVariantMap initialProperties{
        {QStringLiteral("controller"),
         QVariant::fromValue(static_cast<QObject*>(&controller))},
        {QStringLiteral("width"), 1200},
        {QStringLiteral("height"), 900}};
    QQuickWindow window;
    window.setGeometry(0, 0, 1200, 900);
    std::unique_ptr<QObject> object{
        component.createWithInitialProperties(initialProperties)};
    ASSERT_NE(object, nullptr) << component.errorString().toStdString();
    auto* rootItem = qobject_cast<QQuickItem*>(object.get());
    ASSERT_NE(rootItem, nullptr);
    rootItem->setParentItem(window.contentItem());
    window.show();
    for (int attempt = 0; attempt < 30; ++attempt) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    const QStringList controls{
        QStringLiteral("editorVisualXField"),
        QStringLiteral("editorVisualYField"),
        QStringLiteral("editorVisualWidthField"),
        QStringLiteral("editorVisualHeightField"),
        QStringLiteral("editorVisualScaleXField"),
        QStringLiteral("editorVisualScaleYField"),
        QStringLiteral("editorVisualRotationField"),
        QStringLiteral("editorVisualCropLeftField"),
        QStringLiteral("editorVisualCropTopField"),
        QStringLiteral("editorVisualCropRightField"),
        QStringLiteral("editorVisualCropBottomField"),
        QStringLiteral("editorVisualOpacityField"),
        QStringLiteral("editorVisualZOrderField"),
        QStringLiteral("editorVisualApplyButton"),
        QStringLiteral("editorVisualResetButton"),
        QStringLiteral("editorPipFullFrameButton"),
        QStringLiteral("editorPipTopLeftButton"),
        QStringLiteral("editorPipTopRightButton"),
        QStringLiteral("editorPipBottomLeftButton"),
        QStringLiteral("editorPipBottomRightButton"),
        QStringLiteral("editorPipStateLabel"),
        QStringLiteral("editorAudioGainField"),
        QStringLiteral("editorAudioFadeInField"),
        QStringLiteral("editorAudioFadeOutField"),
        QStringLiteral("editorAudioApplyButton"),
        QStringLiteral("editorAudioResetButton"),
        QStringLiteral("editorTitleTextField"),
        QStringLiteral("editorTitleFontField"),
        QStringLiteral("editorTitleResolvedFontLabel"),
        QStringLiteral("editorTitleXField"),
        QStringLiteral("editorTitleYField"),
        QStringLiteral("editorTitleForegroundField"),
        QStringLiteral("editorTitleBackgroundField"),
        QStringLiteral("editorTitleAlignmentBox"),
        QStringLiteral("editorTitleAddButton"),
        QStringLiteral("editorTitleApplyButton"),
        QStringLiteral("editorTitleRemoveButton"),
        QStringLiteral("editorCaptionCueList"),
        QStringLiteral("editorCaptionStartField"),
        QStringLiteral("editorCaptionDurationField"),
        QStringLiteral("editorCaptionTextField"),
        QStringLiteral("editorCaptionAddButton"),
        QStringLiteral("editorCaptionApplyButton"),
        QStringLiteral("editorCaptionRemoveButton"),
    };
    for (const auto& name : controls) {
        auto* control = object->findChild<QObject*>(name);
        ASSERT_NE(control, nullptr) << name.toStdString();
        auto* accessible = QAccessible::queryAccessibleInterface(control);
        ASSERT_NE(accessible, nullptr) << name.toStdString();
        EXPECT_FALSE(accessible->text(QAccessible::Name).isEmpty())
            << name.toStdString();
    }
    auto* visualApply =
        object->findChild<QObject*>(QStringLiteral("editorVisualApplyButton"));
    auto* audioApply =
        object->findChild<QObject*>(QStringLiteral("editorAudioApplyButton"));
    auto* titleApply =
        object->findChild<QObject*>(QStringLiteral("editorTitleApplyButton"));
    auto* captionApply =
        object->findChild<QObject*>(QStringLiteral("editorCaptionApplyButton"));
    EXPECT_FALSE(visualApply->property("enabled").toBool());
    EXPECT_FALSE(audioApply->property("enabled").toBool());
    EXPECT_FALSE(titleApply->property("enabled").toBool());
    EXPECT_FALSE(captionApply->property("enabled").toBool());

    auto* videoClip =
        findVisualItem(rootItem, QStringLiteral("timelineClip-clip-1"));
    ASSERT_NE(videoClip, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(videoClip, "activateSelection"));
    QCoreApplication::processEvents();
    EXPECT_TRUE(visualApply->property("enabled").toBool());
    EXPECT_TRUE(audioApply->property("enabled").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(visualApply, "clicked"));
    for (const auto& name : {QStringLiteral("editorPipFullFrameButton"),
                             QStringLiteral("editorPipTopLeftButton"),
                             QStringLiteral("editorPipTopRightButton"),
                             QStringLiteral("editorPipBottomLeftButton"),
                             QStringLiteral("editorPipBottomRightButton")}) {
        ASSERT_TRUE(QMetaObject::invokeMethod(
            object->findChild<QObject*>(name), "clicked"));
    }
    ASSERT_TRUE(QMetaObject::invokeMethod(audioApply, "clicked"));

    controller.selectClip(QStringLiteral("title-1"), QStringLiteral("title-ui"));
    QCoreApplication::processEvents();
    EXPECT_FALSE(audioApply->property("enabled").toBool());
    EXPECT_TRUE(titleApply->property("enabled").toBool());
    for (const auto& name : {QStringLiteral("editorTitleAddButton"),
                             QStringLiteral("editorTitleApplyButton"),
                             QStringLiteral("editorTitleRemoveButton")}) {
        auto* button = object->findChild<QObject*>(name);
        ASSERT_TRUE(button->property("enabled").toBool()) << name.toStdString();
        ASSERT_TRUE(QMetaObject::invokeMethod(button, "clicked"));
    }

    controller.selectClip(QStringLiteral("caption-1"),
                          QStringLiteral("caption-ui"));
    QCoreApplication::processEvents();
    EXPECT_TRUE(captionApply->property("enabled").toBool());
    for (const auto& name : {QStringLiteral("editorCaptionAddButton"),
                             QStringLiteral("editorCaptionApplyButton"),
                             QStringLiteral("editorCaptionRemoveButton")}) {
        auto* button = object->findChild<QObject*>(name);
        ASSERT_TRUE(button->property("enabled").toBool()) << name.toStdString();
        ASSERT_TRUE(QMetaObject::invokeMethod(button, "clicked"));
    }
    EXPECT_EQ(controller.visualApplyCalls(), 1);
    EXPECT_EQ(controller.pipPresetCalls(), 5);
    EXPECT_EQ(controller.audioApplyCalls(), 1);
    EXPECT_EQ(controller.titleAddCalls(), 1);
    EXPECT_EQ(controller.titleEditCalls(), 1);
    EXPECT_EQ(controller.titleRemoveCalls(), 1);
    EXPECT_EQ(controller.captionAddCalls(), 1);
    EXPECT_EQ(controller.captionEditCalls(), 1);
    EXPECT_EQ(controller.captionRemoveCalls(), 1);

    auto* captionStart = object->findChild<QObject*>(
        QStringLiteral("editorCaptionStartField"));
    ASSERT_TRUE(captionStart->setProperty("text", QStringLiteral("abc")));
    QCoreApplication::processEvents();
    EXPECT_FALSE(captionApply->property("enabled").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(captionApply, "clicked"));
    EXPECT_EQ(controller.captionEditCalls(), 1);

    auto* titleLabel = findVisualItem(
        rootItem, QStringLiteral("timelineClipLabel-title-ui"));
    auto* captionLabel = findVisualItem(
        rootItem, QStringLiteral("timelineClipLabel-caption-ui"));
    ASSERT_NE(titleLabel, nullptr);
    ASSERT_NE(captionLabel, nullptr);
    EXPECT_TRUE(titleLabel->property("text").toString().contains(
        QStringLiteral("TITLE")));
    EXPECT_TRUE(titleLabel->property("text").toString().contains(
        QString::fromUtf8("강의 제목")));
    EXPECT_TRUE(captionLabel->property("text").toString().contains(
        QStringLiteral("CAPTION")));
    auto* captionText = object->findChild<QObject*>(
        QStringLiteral("editorCaptionTextField"));
    ASSERT_NE(captionText, nullptr);
    EXPECT_EQ(captionText->property("text").toString(),
              QString::fromUtf8("첫 자막"));
    controller.selectClip(QStringLiteral("title-1"), QStringLiteral("title-ui"));
    QCoreApplication::processEvents();
    auto* resolvedFont = object->findChild<QObject*>(
        QStringLiteral("editorTitleResolvedFontLabel"));
    ASSERT_NE(resolvedFont, nullptr);
    EXPECT_TRUE(resolvedFont->property("text").toString().contains(
        QStringLiteral("Noto Sans CJK KR")));

    auto* xField =
        object->findChild<QObject*>(QStringLiteral("editorVisualXField"));
    controller.selectClip(QStringLiteral("v1"), QStringLiteral("clip-1"));
    QCoreApplication::processEvents();
    ASSERT_TRUE(xField->setProperty("text", QString{}));
    QCoreApplication::processEvents();
    EXPECT_FALSE(visualApply->property("enabled").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(visualApply, "clicked"));
    EXPECT_EQ(controller.visualApplyCalls(), 1);

    controller.setBusy(true);
    QCoreApplication::processEvents();
    EXPECT_FALSE(visualApply->property("enabled").toBool());
    EXPECT_FALSE(object->findChild<QObject*>(
        QStringLiteral("editorTitleAddButton"))->property("enabled").toBool());
    EXPECT_FALSE(object->findChild<QObject*>(
        QStringLiteral("editorCaptionAddButton"))->property("enabled").toBool());
    controller.setBusy(false);
    controller.setSessionBusy(true);
    QCoreApplication::processEvents();
    EXPECT_FALSE(visualApply->property("enabled").toBool());
    EXPECT_FALSE(object->findChild<QObject*>(
        QStringLiteral("editorTitleAddButton"))->property("enabled").toBool());
    controller.setSessionBusy(false);
    controller.selectClip(QStringLiteral("a1"), QStringLiteral("audio-ui"));
    QCoreApplication::processEvents();
    EXPECT_FALSE(visualApply->property("enabled").toBool());
    EXPECT_TRUE(audioApply->property("enabled").toBool());
    controller.clearSelection();
    QCoreApplication::processEvents();
    EXPECT_FALSE(visualApply->property("enabled").toBool());
    EXPECT_FALSE(audioApply->property("enabled").toBool());

    controller.selectClip(QStringLiteral("v1"), QStringLiteral("clip-1"));
    QCoreApplication::processEvents();
    ASSERT_TRUE(QMetaObject::invokeMethod(xField, "forceActiveFocus"));
    QCoreApplication::processEvents();
    auto* splitShortcut =
        object->findChild<QObject*>(QStringLiteral("editorSplitShortcut"));
    ASSERT_NE(splitShortcut, nullptr);
    EXPECT_FALSE(splitShortcut->property("enabled").toBool());
}

TEST(QmlSmokeTest, StudioPageShowsCaptureTargetsAndTerminalError) {
    QQmlEngine engine;
    FakeStudioController studioController;
    FakeScreenCaptureController screenCaptureController;
    FakeDeviceCaptureController deviceCaptureController;
    FakeStudioWorkflowController studioWorkflowController;
    FakeShortcutSettingsController shortcutSettingsController;
    engine.rootContext()->setContextProperty(QStringLiteral("studioController"),
                                             &studioController);
    engine.rootContext()->setContextProperty(QStringLiteral("screenCaptureController"),
                                             &screenCaptureController);
    engine.rootContext()->setContextProperty(QStringLiteral("deviceCaptureController"),
                                             &deviceCaptureController);
    engine.rootContext()->setContextProperty(
        QStringLiteral("studioWorkflowController"), &studioWorkflowController);
    engine.rootContext()->setContextProperty(
        QStringLiteral("shortcutSettingsController"), &shortcutSettingsController);
    QQmlComponent component{
        &engine, QUrl::fromLocalFile(QString::fromUtf8(CS_QML_SOURCE_DIR "/StudioPage.qml"))};

    std::unique_ptr<QObject> object{component.create()};

    ASSERT_NE(object, nullptr) << component.errorString().toStdString();
    auto* selector = object->findChild<QObject*>(QStringLiteral("captureTargetSelector"));
    auto* status = object->findChild<QObject*>(QStringLiteral("captureStatusLabel"));
    auto* preview = object->findChild<QObject*>(
        QStringLiteral("studioScreenCompositionPreview"));
    auto* cameraSelector = object->findChild<QObject*>(QStringLiteral("cameraDeviceSelector"));
    auto* microphoneSelector =
        object->findChild<QObject*>(QStringLiteral("microphoneDeviceSelector"));
    auto* microphoneMeter = object->findChild<QObject*>(QStringLiteral("microphoneLevelMeter"));
    auto* disk = object->findChild<QObject*>(QStringLiteral("recordingDiskLabel"));
    auto* encoder = object->findChild<QObject*>(QStringLiteral("recordingEncoderLabel"));
    auto* queue = object->findChild<QObject*>(QStringLiteral("recordingQueueLabel"));
    auto* sync = object->findChild<QObject*>(QStringLiteral("recordingSyncLabel"));
    ASSERT_NE(selector, nullptr);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(preview, nullptr);
    ASSERT_NE(cameraSelector, nullptr);
    ASSERT_NE(microphoneSelector, nullptr);
    ASSERT_NE(microphoneMeter, nullptr);
    ASSERT_NE(disk, nullptr);
    ASSERT_NE(encoder, nullptr);
    ASSERT_NE(queue, nullptr);
    ASSERT_NE(sync, nullptr);
    EXPECT_TRUE(disk->property("text").toString().contains(QStringLiteral("8.0 GiB")));
    EXPECT_TRUE(encoder->property("text").toString().contains(QStringLiteral("mpeg4, aac")));
    EXPECT_TRUE(queue->property("text").toString().contains(QStringLiteral("Queue: 3")));
    EXPECT_TRUE(sync->property("text").toString().contains(QStringLiteral("duplicate 4")));
    EXPECT_TRUE(sync->property("text").toString().contains(QStringLiteral("8.5 ms")));
    EXPECT_EQ(selector->property("count").toInt(), 1);
    EXPECT_EQ(status->property("text").toString(),
              QStringLiteral("captured window closed"));
    EXPECT_EQ(cameraSelector->property("count").toInt(), 1);
    EXPECT_EQ(microphoneSelector->property("count").toInt(), 1);
    EXPECT_GT(microphoneMeter->property("value").toDouble(), 0.0);
}

TEST(QmlSmokeTest, StudioShortcutsShareVisibleActionsAndStateGuards) {
    QQmlEngine engine;
    FakeStudioController studioController;
    FakeScreenCaptureController screenCaptureController;
    FakeDeviceCaptureController deviceCaptureController;
    FakeStudioWorkflowController studioWorkflowController;
    FakeShortcutSettingsController shortcutSettingsController;
    studioController.setRecording(true);
    engine.rootContext()->setContextProperty(QStringLiteral("studioController"),
                                             &studioController);
    engine.rootContext()->setContextProperty(QStringLiteral("screenCaptureController"),
                                             &screenCaptureController);
    engine.rootContext()->setContextProperty(QStringLiteral("deviceCaptureController"),
                                             &deviceCaptureController);
    engine.rootContext()->setContextProperty(
        QStringLiteral("studioWorkflowController"), &studioWorkflowController);
    engine.rootContext()->setContextProperty(
        QStringLiteral("shortcutSettingsController"), &shortcutSettingsController);
    QQmlComponent component{
        &engine,
        QUrl::fromLocalFile(QString::fromUtf8(CS_QML_SOURCE_DIR "/StudioPage.qml"))};
    std::unique_ptr<QObject> object{component.create()};
    ASSERT_NE(object, nullptr) << component.errorString().toStdString();
    QCoreApplication::processEvents();

    const auto proveParity = [&](const QString& actionName,
                                 const QString& buttonName,
                                 const QString& shortcutName,
                                 const std::function<void()>& reset,
                                 const std::function<int()>& calls) {
        auto* action = object->findChild<QObject*>(actionName);
        auto* button = object->findChild<QObject*>(buttonName);
        auto* shortcut = object->findChild<QObject*>(shortcutName);
        ASSERT_NE(action, nullptr) << actionName.toStdString();
        ASSERT_NE(button, nullptr) << buttonName.toStdString();
        ASSERT_NE(shortcut, nullptr) << shortcutName.toStdString();
        EXPECT_EQ(button->property("action").value<QObject*>(), action);
        ASSERT_TRUE(action->property("enabled").toBool());
        ASSERT_TRUE(shortcut->property("enabled").toBool());
        reset();
        const int before = calls();
        ASSERT_TRUE(QMetaObject::invokeMethod(action, "trigger"));
        const int afterAction = calls();
        EXPECT_EQ(afterAction, before + 1);
        reset();
        ASSERT_TRUE(QMetaObject::invokeMethod(shortcut, "activated"));
        EXPECT_EQ(calls(), afterAction + 1);
    };

    proveParity(QStringLiteral("studioMarkerAction"),
                QStringLiteral("studioMarkerButton"),
                QStringLiteral("studioMarkerShortcut"), [] {},
                [&] { return studioWorkflowController.markerCalls(); });
    EXPECT_EQ(studioWorkflowController.lastPositionNs(), 777);

    proveParity(QStringLiteral("studioPreviousSceneAction"),
                QStringLiteral("studioPreviousSceneButton"),
                QStringLiteral("studioPreviousSceneShortcut"),
                [&] { studioWorkflowController.setActiveSceneId(QStringLiteral("scene-5")); },
                [&] { return studioWorkflowController.switchCalls(); });
    EXPECT_EQ(studioWorkflowController.lastSceneId(), QStringLiteral("scene-4"));
    proveParity(QStringLiteral("studioNextSceneAction"),
                QStringLiteral("studioNextSceneButton"),
                QStringLiteral("studioNextSceneShortcut"),
                [&] { studioWorkflowController.setActiveSceneId(QStringLiteral("scene-5")); },
                [&] { return studioWorkflowController.switchCalls(); });
    EXPECT_EQ(studioWorkflowController.lastSceneId(), QStringLiteral("scene-6"));

    for (int scene = 1; scene <= 9; ++scene) {
        proveParity(QStringLiteral("studioScene%1Action").arg(scene),
                    QStringLiteral("studioSceneButton%1").arg(scene),
                    QStringLiteral("studioScene%1Shortcut").arg(scene), [] {},
                    [&] { return studioWorkflowController.switchCalls(); });
        EXPECT_EQ(studioWorkflowController.lastSceneId(),
                  QStringLiteral("scene-%1").arg(scene));
    }

    studioWorkflowController.setBusy(true);
    QCoreApplication::processEvents();
    for (const QString& name :
         {QStringLiteral("studioMarkerAction"),
          QStringLiteral("studioPreviousSceneAction"),
          QStringLiteral("studioNextSceneAction"),
          QStringLiteral("studioScene1Action")}) {
        EXPECT_FALSE(object->findChild<QObject*>(name)->property("enabled").toBool());
    }
    studioWorkflowController.setBusy(false);
    studioController.setBusy(true);
    QCoreApplication::processEvents();
    for (const QString& name :
         {QStringLiteral("studioMarkerAction"),
          QStringLiteral("studioPreviousSceneAction"),
          QStringLiteral("studioNextSceneAction"),
          QStringLiteral("studioScene1Action")}) {
        EXPECT_FALSE(object->findChild<QObject*>(name)->property("enabled").toBool());
    }
    studioController.setBusy(false);
    studioController.setRecording(false);
    QCoreApplication::processEvents();
    EXPECT_FALSE(object->findChild<QObject*>(QStringLiteral("studioMarkerAction"))
                     ->property("enabled")
                     .toBool());
    studioWorkflowController.setRecording(false);
    QCoreApplication::processEvents();
    EXPECT_FALSE(object->findChild<QObject*>(QStringLiteral("studioMarkerAction"))
                     ->property("enabled")
                     .toBool());
    EXPECT_TRUE(object->findChild<QObject*>(QStringLiteral("studioScene1Action"))
                    ->property("enabled")
                    .toBool());
}

TEST(QmlSmokeTest, StudioPageProvidesModelDrivenAccessibleWorkflowAtAllSizes) {
    const std::array<std::tuple<int, int, qreal>, 4> fixtures{
        std::tuple{1280, 720, 1.0}, std::tuple{1440, 900, 1.0},
        // A 1440x900 display at 200% exposes 720x450 device-independent pixels.
        std::tuple{720, 450, 2.0},
        std::tuple{360, 640, 3.0}};
    for (const auto& [width, height, scale] : fixtures) {
        QQmlEngine engine;
        FakeStudioController studioController;
        FakeScreenCaptureController screenCaptureController;
        FakeDeviceCaptureController deviceCaptureController;
        FakeStudioWorkflowController studioWorkflowController;
        FakeShortcutSettingsController shortcutSettingsController;
        studioWorkflowController.setRecording(false);
        QSignalSpy warningSpy{&engine, &QQmlEngine::warnings};
        engine.rootContext()->setContextProperty(QStringLiteral("studioController"),
                                                 &studioController);
        engine.rootContext()->setContextProperty(
            QStringLiteral("screenCaptureController"), &screenCaptureController);
        engine.rootContext()->setContextProperty(
            QStringLiteral("deviceCaptureController"), &deviceCaptureController);
        engine.rootContext()->setContextProperty(
            QStringLiteral("studioWorkflowController"), &studioWorkflowController);
        engine.rootContext()->setContextProperty(
            QStringLiteral("shortcutSettingsController"), &shortcutSettingsController);
        QQmlComponent component{
            &engine,
            QUrl::fromLocalFile(QString::fromUtf8(CS_QML_SOURCE_DIR "/StudioPage.qml"))};
        QVariantMap initialProperties{{QStringLiteral("width"), width},
                                      {QStringLiteral("height"), height}};
        std::unique_ptr<QObject> object{
            component.createWithInitialProperties(initialProperties)};
        ASSERT_NE(object, nullptr) << component.errorString().toStdString();
        auto* rootItem = qobject_cast<QQuickItem*>(object.get());
        ASSERT_NE(rootItem, nullptr);
        QQuickWindow window;
        window.setGeometry(0, 0, width, height);
        rootItem->setParentItem(window.contentItem());
        window.show();
        for (int attempt = 0; attempt < 20; ++attempt) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(2);
        }

        const QStringList controls{
            QStringLiteral("studioSceneList"),
            QStringLiteral("studioSceneAddField"),
            QStringLiteral("studioSceneAddButton"),
            QStringLiteral("studioSceneDuplicateButton"),
            QStringLiteral("studioSceneRenameField"),
            QStringLiteral("studioSceneRenameButton"),
            QStringLiteral("studioSceneRemoveButton"),
            QStringLiteral("studioSceneUpButton"),
            QStringLiteral("studioSceneDownButton"),
            QStringLiteral("studioSourceList"),
            QStringLiteral("studioSourceToggleButton"),
            QStringLiteral("studioSourceUpButton"),
            QStringLiteral("studioSourceDownButton"),
            QStringLiteral("studioLeftScroll"),
            QStringLiteral("studioTransformXField"),
            QStringLiteral("studioTransformYField"),
            QStringLiteral("studioTransformWidthField"),
            QStringLiteral("studioTransformHeightField"),
            QStringLiteral("studioTransformOpacityField"),
            QStringLiteral("studioTransformZOrderField"),
            QStringLiteral("studioTransformApplyButton"),
            QStringLiteral("studioTransformResetButton"),
            QStringLiteral("studioPipBottomRightButton"),
            QStringLiteral("studioShortcutEditorButton"),
            QStringLiteral("studioRecordShortcutField"),
            QStringLiteral("studioMarkerShortcutField"),
            QStringLiteral("studioScene9ShortcutField"),
            QStringLiteral("studioScreenCompositionPreview"),
            QStringLiteral("studioCameraCompositionPreview"),
            QStringLiteral("studioHudActiveScene"),
            QStringLiteral("studioHudSession"),
            QStringLiteral("studioHudMarkerCount"),
            QStringLiteral("studioHudReconciliation")};
        for (const QString& name : controls) {
            auto* control = object->findChild<QObject*>(name);
            ASSERT_NE(control, nullptr)
                << name.toStdString() << " at " << width << 'x' << height
                << " scale " << scale;
            auto* accessible = QAccessible::queryAccessibleInterface(control);
            ASSERT_NE(accessible, nullptr) << name.toStdString();
            EXPECT_FALSE(accessible->text(QAccessible::Name).isEmpty())
                << name.toStdString();
        }
        QString warningText;
        for (const auto& emission : warningSpy) {
            for (const auto& warning :
                 qvariant_cast<QList<QQmlError>>(emission.front())) {
                warningText += warning.toString() + QLatin1Char('\n');
            }
        }
        EXPECT_EQ(warningSpy.count(), 0) << warningText.toStdString();

        if (width < 700) {
            auto* tabs = object->findChild<QObject*>(
                QStringLiteral("studioCompactTabs"));
            ASSERT_NE(tabs, nullptr);
            EXPECT_TRUE(tabs->property("visible").toBool());
            rootItem->setProperty("compactSection", QStringLiteral("capture"));
            QCoreApplication::processEvents();
        }

        auto* leftScroll = qobject_cast<QQuickItem*>(object->findChild<QObject*>(
            QStringLiteral("studioLeftScroll")));
        ASSERT_NE(leftScroll, nullptr);
        const QPointF leftTop = leftScroll->mapToItem(rootItem, QPointF{});
        const QPointF leftBottom = leftScroll->mapToItem(
            rootItem, QPointF{leftScroll->width(), leftScroll->height()});
        EXPECT_GE(leftTop.x(), 0.0);
        EXPECT_GE(leftTop.y(), 0.0);
        EXPECT_LE(leftBottom.x(), rootItem->width() + 0.5);
        EXPECT_LE(leftBottom.y(), rootItem->height() + 0.5);
        auto* leftContent = leftScroll->property("contentItem").value<QObject*>();
        ASSERT_NE(leftContent, nullptr);
        const double leftContentHeight =
            leftScroll->property("contentHeight").toDouble();
        if (height <= 720) {
            EXPECT_GT(leftContentHeight, leftScroll->height())
                << "the device controls need a reachable vertical scroll range";
        }
        leftContent->setProperty(
            "contentY", leftContentHeight - leftScroll->height());
        QCoreApplication::processEvents();
        auto* systemAudioButton = object->findChild<QObject*>(
            QStringLiteral("studioSystemAudioButton"));
        ASSERT_NE(systemAudioButton, nullptr);
        auto* systemAudioAccessible =
            QAccessible::queryAccessibleInterface(systemAudioButton);
        ASSERT_NE(systemAudioAccessible, nullptr);
        EXPECT_FALSE(systemAudioAccessible->text(QAccessible::Name).isEmpty());

        auto* cameraComposition = qobject_cast<QQuickItem*>(
            object->findChild<QObject*>(
                QStringLiteral("studioCameraCompositionPreview")));
        if (width < 700) {
            rootItem->setProperty("compactSection", QStringLiteral("preview"));
            QCoreApplication::processEvents();
        }
        ASSERT_NE(cameraComposition, nullptr);
        auto* canvas = cameraComposition->parentItem();
        ASSERT_NE(canvas, nullptr);
        EXPECT_NEAR(cameraComposition->x(), canvas->width() * 0.61, 0.5);
        EXPECT_DOUBLE_EQ(cameraComposition->property("cropLeft").toDouble(), 0.1);
        EXPECT_DOUBLE_EQ(cameraComposition->property("cropTop").toDouble(), 0.05);
        EXPECT_DOUBLE_EQ(cameraComposition->property("cropRight").toDouble(), 0.2);
        EXPECT_DOUBLE_EQ(cameraComposition->property("cropBottom").toDouble(), 0.15);
        EXPECT_DOUBLE_EQ(cameraComposition->property("scaleX").toDouble(), 1.2);
        EXPECT_DOUBLE_EQ(cameraComposition->property("scaleY").toDouble(), 0.8);
        const QPointF transformedCenter = cameraComposition->mapToItem(
            canvas, QPointF{cameraComposition->width() / 2,
                            cameraComposition->height() / 2});
        EXPECT_NEAR(transformedCenter.x(),
                    cameraComposition->x() + cameraComposition->width() / 2,
                    0.01);
        EXPECT_NEAR(transformedCenter.y(),
                    cameraComposition->y() + cameraComposition->height() / 2,
                    0.01);
        auto* screenComposition = qobject_cast<QQuickItem*>(
            object->findChild<QObject*>(
                QStringLiteral("studioScreenCompositionPreview")));
        ASSERT_NE(screenComposition, nullptr);
        EXPECT_GT(cameraComposition->z(), screenComposition->z());

        EXPECT_TRUE(object->findChild<QObject*>(
                              QStringLiteral("studioHudActiveScene"))
                        ->property("text")
                        .toString()
                        .contains(QStringLiteral("scene-5")));
        EXPECT_TRUE(object->findChild<QObject*>(
                              QStringLiteral("studioHudSession"))
                        ->property("text")
                        .toString()
                        .contains(QStringLiteral("abcdef")));
        EXPECT_TRUE(object->findChild<QObject*>(
                              QStringLiteral("studioHudMarkerCount"))
                        ->property("text")
                        .toString()
                        .contains(QStringLiteral("3")));
        EXPECT_TRUE(object->findChild<QObject*>(
                              QStringLiteral("studioHudReconciliation"))
                        ->property("text")
                        .toString()
                        .contains(QStringLiteral("Not active")));

        if (width == 1280 && scale == 1.0) {
            auto* widthField = object->findChild<QObject*>(
                QStringLiteral("studioTransformWidthField"));
            auto* apply = object->findChild<QObject*>(
                QStringLiteral("studioTransformApplyButton"));
            ASSERT_NE(widthField, nullptr);
            ASSERT_NE(apply, nullptr);
            widthField->setProperty("text", QStringLiteral("0"));
            QCoreApplication::processEvents();
            EXPECT_FALSE(apply->property("enabled").toBool());
            widthField->setProperty("text", QStringLiteral("0.25"));
            QCoreApplication::processEvents();
            EXPECT_TRUE(apply->property("enabled").toBool());
            ASSERT_TRUE(QMetaObject::invokeMethod(apply, "clicked"));
            EXPECT_EQ(studioWorkflowController.transformCalls(), 1);

            auto* addField = object->findChild<QObject*>(
                QStringLiteral("studioSceneAddField"));
            auto* addButton = object->findChild<QObject*>(
                QStringLiteral("studioSceneAddButton"));
            addField->setProperty("text", QString::fromUtf8("강의 장면"));
            QCoreApplication::processEvents();
            ASSERT_TRUE(addButton->property("enabled").toBool());
            ASSERT_TRUE(QMetaObject::invokeMethod(addButton, "clicked"));
            EXPECT_EQ(studioWorkflowController.sceneEditCalls(), 1);
            auto* toggle = object->findChild<QObject*>(
                QStringLiteral("studioSourceToggleButton"));
            ASSERT_TRUE(QMetaObject::invokeMethod(toggle, "clicked"));
            EXPECT_EQ(studioWorkflowController.sourceEditCalls(), 1);

            auto* markerShortcutField = object->findChild<QObject*>(
                QStringLiteral("studioMarkerShortcutField"));
            markerShortcutField->setProperty("text", QStringLiteral("Alt+M"));
            ASSERT_TRUE(QMetaObject::invokeMethod(markerShortcutField,
                                                  "editingFinished"));
            EXPECT_EQ(shortcutSettingsController.setCalls(), 1);

            auto* scene9Action = object->findChild<QObject*>(
                QStringLiteral("studioScene9Action"));
            ASSERT_TRUE(scene9Action->property("enabled").toBool());
            studioWorkflowController.setSceneCount(3);
            QCoreApplication::processEvents();
            EXPECT_FALSE(scene9Action->property("enabled").toBool());

            studioWorkflowController.setSelectedSourceId(
                QStringLiteral("microphone"));
            QCoreApplication::processEvents();
            EXPECT_FALSE(widthField->property("enabled").toBool());
            EXPECT_FALSE(apply->property("enabled").toBool());
            studioWorkflowController.setSelectedSourceId(
                QStringLiteral("camera"));
            QCoreApplication::processEvents();
            EXPECT_TRUE(widthField->property("enabled").toBool());

            studioWorkflowController.setRecording(true);
            QCoreApplication::processEvents();
            EXPECT_FALSE(widthField->property("enabled").toBool());
            EXPECT_FALSE(addButton->property("enabled").toBool());
            EXPECT_FALSE(toggle->property("enabled").toBool());
            studioWorkflowController.setRecording(false);
            QCoreApplication::processEvents();

            studioController.setRecording(true);
            QCoreApplication::processEvents();
            EXPECT_FALSE(widthField->property("enabled").toBool());
            EXPECT_FALSE(addButton->property("enabled").toBool());
            EXPECT_FALSE(toggle->property("enabled").toBool());
            EXPECT_TRUE(object->findChild<QObject*>(
                                   QStringLiteral("studioScene1Action"))
                            ->property("enabled")
                            .toBool());
            studioController.setBusy(true);
            QCoreApplication::processEvents();
            EXPECT_FALSE(object->findChild<QObject*>(
                                    QStringLiteral("studioScene1Action"))
                             ->property("enabled")
                             .toBool());
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app{argc, argv};
    qmlRegisterType<creator::app::ScreenPreviewItem>("CreatorStudio.Native", 1, 0,
                                                      "ScreenPreviewItem");
    qmlRegisterType<creator::app::CameraPreviewItem>("CreatorStudio.Native", 1, 0,
                                                      "CameraPreviewItem");
    qmlRegisterType<creator::app::EditorPreviewItem>("CreatorStudio.Native", 1, 0,
                                                      "EditorPreviewItem");
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

#include "QmlSmokeTest.moc"
