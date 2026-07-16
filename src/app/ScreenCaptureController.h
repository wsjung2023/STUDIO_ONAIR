#pragma once

#include "capture/IScreenCaptureSource.h"
#include "capture/IScreenCaptureDiscovery.h"
#include "capture/IScreenCapturePermission.h"
#include "capture/IScreenCaptureSourceFactory.h"
#include "capture/LatestVideoFrameMailbox.h"
#include "capture/ScreenCaptureTypes.h"
#include "core/Result.h"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include <cstdint>
#include <memory>
#include <vector>

namespace creator::app {

enum class ScreenCaptureState {
    Idle,
    PermissionRequired,
    CheckingPermission,
    Discovering,
    Ready,
    Starting,
    Previewing,
    Stopping,
    Error,
};

/// QML-facing coordinator for screen permission, target discovery, and preview.
///
/// Native callbacks are always queued back to this object's thread. Frames do
/// not pass through QObject signals: the source publishes to a bounded mailbox
/// that a render-thread preview item can consume independently.
class ScreenCaptureController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY captureStateChanged)
    Q_PROPERTY(bool previewing READ previewing NOTIFY captureStateChanged)
    Q_PROPERTY(bool canStopPreview READ canStopPreview NOTIFY captureStateChanged)
    Q_PROPERTY(bool permissionRequired READ permissionRequired NOTIFY captureStateChanged)
    Q_PROPERTY(QVariantList targets READ targets NOTIFY targetsChanged)
    Q_PROPERTY(QString selectedTargetId READ selectedTargetId NOTIFY selectedTargetChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(quint32 actualWidth READ actualWidth NOTIFY statsChanged)
    Q_PROPERTY(quint32 actualHeight READ actualHeight NOTIFY statsChanged)
    Q_PROPERTY(qulonglong receivedFrames READ receivedFrames NOTIFY statsChanged)
    Q_PROPERTY(qulonglong droppedFrames READ droppedFrames NOTIFY statsChanged)
    Q_PROPERTY(qulonglong ignoredFrames READ ignoredFrames NOTIFY statsChanged)
    Q_PROPERTY(qulonglong invalidFrames READ invalidFrames NOTIFY statsChanged)
    Q_PROPERTY(qulonglong replacedPreviewFrames READ replacedPreviewFrames NOTIFY statsChanged)
    Q_PROPERTY(double currentFps READ currentFps NOTIFY statsChanged)

public:
    ScreenCaptureController(
        std::unique_ptr<creator::capture::IScreenCapturePermission> permission,
        std::unique_ptr<creator::capture::IScreenCaptureDiscovery> discovery,
        std::unique_ptr<creator::capture::IScreenCaptureSourceFactory> sourceFactory,
        QObject* parent = nullptr);
    ~ScreenCaptureController() override;

    [[nodiscard]] ScreenCaptureState state() const noexcept { return state_; }
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool previewing() const noexcept {
        return state_ == ScreenCaptureState::Previewing;
    }
    [[nodiscard]] bool canStopPreview() const noexcept {
        return state_ == ScreenCaptureState::Starting ||
               state_ == ScreenCaptureState::Previewing;
    }
    [[nodiscard]] bool permissionRequired() const noexcept {
        return state_ == ScreenCaptureState::PermissionRequired;
    }
    [[nodiscard]] QVariantList targets() const { return targetModel_; }
    [[nodiscard]] QString selectedTargetId() const { return selectedTargetId_; }
    [[nodiscard]] QString statusMessage() const { return statusMessage_; }
    [[nodiscard]] quint32 actualWidth() const noexcept { return actualWidth_; }
    [[nodiscard]] quint32 actualHeight() const noexcept { return actualHeight_; }
    [[nodiscard]] qulonglong receivedFrames() const noexcept { return receivedFrames_; }
    [[nodiscard]] qulonglong droppedFrames() const noexcept { return droppedFrames_; }
    [[nodiscard]] qulonglong ignoredFrames() const noexcept { return ignoredFrames_; }
    [[nodiscard]] qulonglong invalidFrames() const noexcept { return invalidFrames_; }
    [[nodiscard]] qulonglong replacedPreviewFrames() const noexcept {
        return replacedPreviewFrames_;
    }
    [[nodiscard]] double currentFps() const noexcept { return currentFps_; }

    [[nodiscard]] std::shared_ptr<creator::capture::LatestVideoFrameMailbox>
    previewMailbox() const noexcept {
        return mailbox_;
    }

    Q_INVOKABLE void initialize();
    Q_INVOKABLE void requestPermission();
    Q_INVOKABLE void refreshTargets();
    Q_INVOKABLE void selectTarget(const QString& targetId);
    Q_INVOKABLE void startPreview();
    Q_INVOKABLE void stopPreview();

public slots:
    void pollCapture();

signals:
    void captureStateChanged();
    void targetsChanged();
    void selectedTargetChanged();
    void statusMessageChanged();
    void statsChanged();

private:
    void beginDiscovery();
    void handlePermissionResult(
        std::uint64_t generation,
        creator::core::Result<creator::capture::ScreenCapturePermissionStatus> result);
    void handleDiscoveryResult(
        std::uint64_t generation,
        creator::core::Result<std::vector<creator::capture::ScreenCaptureTarget>> result);
    void handleStopResult(std::uint64_t generation,
                          creator::core::Result<void> result);
    void setState(ScreenCaptureState state);
    void setStatusMessage(QString message);
    void rebuildTargetModel();
    void updateStats();
    void releaseSource() noexcept;
    [[nodiscard]] const creator::capture::ScreenCaptureTarget* selectedTarget() const;

    std::unique_ptr<creator::capture::IScreenCapturePermission> permission_;
    std::unique_ptr<creator::capture::IScreenCaptureDiscovery> discovery_;
    std::unique_ptr<creator::capture::IScreenCaptureSourceFactory> sourceFactory_;
    std::unique_ptr<creator::capture::IScreenCaptureSource> source_;
    std::shared_ptr<creator::capture::LatestVideoFrameMailbox> mailbox_;
    std::vector<creator::capture::ScreenCaptureTarget> targetSnapshot_;
    QVariantList targetModel_;
    QString selectedTargetId_;
    QString statusMessage_;
    QTimer pollTimer_;
    ScreenCaptureState state_{ScreenCaptureState::Idle};
    std::uint64_t generation_{0};
    quint32 actualWidth_{0};
    quint32 actualHeight_{0};
    qulonglong receivedFrames_{0};
    qulonglong droppedFrames_{0};
    qulonglong ignoredFrames_{0};
    qulonglong invalidFrames_{0};
    qulonglong replacedPreviewFrames_{0};
    double currentFps_{0.0};
};

}  // namespace creator::app
