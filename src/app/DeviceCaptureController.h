#pragma once

#include "capture/AudioCaptureMailbox.h"
#include "capture/AudioLevelMeter.h"
#include "capture/DeviceCaptureTypes.h"
#include "capture/IDeviceCaptureBackend.h"
#include "capture/IDeviceCaptureSource.h"
#include "capture/LatestVideoFrameMailbox.h"
#include "core/Result.h"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include <cstdint>
#include <memory>
#include <vector>

namespace creator::app {

enum class DeviceCaptureState {
    Disabled,
    PermissionRequired,
    Ready,
    Starting,
    Capturing,
    Stopping,
    Error,
};

/// QML-facing owner of independent camera, microphone, and system-audio slots.
class DeviceCaptureController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList cameras READ cameras NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList microphones READ microphones NOTIFY devicesChanged)
    Q_PROPERTY(QString selectedCameraId READ selectedCameraId NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedMicrophoneId READ selectedMicrophoneId NOTIFY selectionChanged)
    Q_PROPERTY(bool cameraPermissionRequired READ cameraPermissionRequired NOTIFY stateChanged)
    Q_PROPERTY(bool microphonePermissionRequired READ microphonePermissionRequired NOTIFY stateChanged)
    Q_PROPERTY(bool cameraCapturing READ cameraCapturing NOTIFY stateChanged)
    Q_PROPERTY(bool microphoneCapturing READ microphoneCapturing NOTIFY stateChanged)
    Q_PROPERTY(bool systemAudioCapturing READ systemAudioCapturing NOTIFY stateChanged)
    Q_PROPERTY(bool cameraBusy READ cameraBusy NOTIFY stateChanged)
    Q_PROPERTY(bool microphoneBusy READ microphoneBusy NOTIFY stateChanged)
    Q_PROPERTY(bool systemAudioBusy READ systemAudioBusy NOTIFY stateChanged)
    Q_PROPERTY(QString cameraStatus READ cameraStatus NOTIFY stateChanged)
    Q_PROPERTY(QString microphoneStatus READ microphoneStatus NOTIFY stateChanged)
    Q_PROPERTY(QString systemAudioStatus READ systemAudioStatus NOTIFY stateChanged)
    Q_PROPERTY(quint32 cameraWidth READ cameraWidth NOTIFY statsChanged)
    Q_PROPERTY(quint32 cameraHeight READ cameraHeight NOTIFY statsChanged)
    Q_PROPERTY(double cameraFps READ cameraFps NOTIFY statsChanged)
    Q_PROPERTY(double microphonePeakDbfs READ microphonePeakDbfs NOTIFY statsChanged)
    Q_PROPERTY(double microphoneRmsDbfs READ microphoneRmsDbfs NOTIFY statsChanged)
    Q_PROPERTY(double systemAudioPeakDbfs READ systemAudioPeakDbfs NOTIFY statsChanged)
    Q_PROPERTY(double systemAudioRmsDbfs READ systemAudioRmsDbfs NOTIFY statsChanged)
    Q_PROPERTY(qulonglong microphoneBlocks READ microphoneBlocks NOTIFY statsChanged)
    Q_PROPERTY(qulonglong systemAudioBlocks READ systemAudioBlocks NOTIFY statsChanged)
    Q_PROPERTY(qulonglong microphoneOverruns READ microphoneOverruns NOTIFY statsChanged)
    Q_PROPERTY(qulonglong systemAudioOverruns READ systemAudioOverruns NOTIFY statsChanged)

public:
    explicit DeviceCaptureController(
        std::unique_ptr<creator::capture::IDeviceCaptureBackend> backend,
        QObject* parent = nullptr);
    ~DeviceCaptureController() override;

    [[nodiscard]] QVariantList cameras() const { return cameraModel_; }
    [[nodiscard]] QVariantList microphones() const { return microphoneModel_; }
    [[nodiscard]] QString selectedCameraId() const { return selectedCameraId_; }
    [[nodiscard]] QString selectedMicrophoneId() const { return selectedMicrophoneId_; }
    [[nodiscard]] bool cameraPermissionRequired() const noexcept;
    [[nodiscard]] bool microphonePermissionRequired() const noexcept;
    [[nodiscard]] bool cameraCapturing() const noexcept;
    [[nodiscard]] bool microphoneCapturing() const noexcept;
    [[nodiscard]] bool systemAudioCapturing() const noexcept;
    [[nodiscard]] bool cameraBusy() const noexcept;
    [[nodiscard]] bool microphoneBusy() const noexcept;
    [[nodiscard]] bool systemAudioBusy() const noexcept;
    [[nodiscard]] DeviceCaptureState cameraState() const noexcept { return cameraState_; }
    [[nodiscard]] DeviceCaptureState microphoneState() const noexcept {
        return microphoneState_;
    }
    [[nodiscard]] DeviceCaptureState systemAudioState() const noexcept {
        return systemAudioState_;
    }
    [[nodiscard]] QString cameraStatus() const { return cameraStatus_; }
    [[nodiscard]] QString microphoneStatus() const { return microphoneStatus_; }
    [[nodiscard]] QString systemAudioStatus() const { return systemAudioStatus_; }
    [[nodiscard]] quint32 cameraWidth() const noexcept { return cameraWidth_; }
    [[nodiscard]] quint32 cameraHeight() const noexcept { return cameraHeight_; }
    [[nodiscard]] double cameraFps() const noexcept { return cameraFps_; }
    [[nodiscard]] double microphonePeakDbfs() const noexcept {
        return microphoneLevel_.peakDbfs;
    }
    [[nodiscard]] double microphoneRmsDbfs() const noexcept {
        return microphoneLevel_.rmsDbfs;
    }
    [[nodiscard]] double systemAudioPeakDbfs() const noexcept {
        return systemAudioLevel_.peakDbfs;
    }
    [[nodiscard]] double systemAudioRmsDbfs() const noexcept {
        return systemAudioLevel_.rmsDbfs;
    }
    [[nodiscard]] qulonglong microphoneBlocks() const noexcept {
        return microphoneBlocks_;
    }
    [[nodiscard]] qulonglong systemAudioBlocks() const noexcept {
        return systemAudioBlocks_;
    }
    [[nodiscard]] qulonglong microphoneOverruns() const noexcept {
        return microphoneOverruns_;
    }
    [[nodiscard]] qulonglong systemAudioOverruns() const noexcept {
        return systemAudioOverruns_;
    }

    Q_INVOKABLE void initialize();
    Q_INVOKABLE void refreshDevices();
    Q_INVOKABLE void requestCameraPermission();
    Q_INVOKABLE void requestMicrophonePermission();
    Q_INVOKABLE void selectCamera(const QString& deviceId);
    Q_INVOKABLE void selectMicrophone(const QString& deviceId);
    Q_INVOKABLE void setCameraEnabled(bool enabled);
    Q_INVOKABLE void setMicrophoneEnabled(bool enabled);
    Q_INVOKABLE void setSystemAudioEnabled(bool enabled);

public slots:
    void pollCapture();

signals:
    void devicesChanged();
    void selectionChanged();
    void stateChanged();
    void statsChanged();

private:
    enum class SlotKind { Camera, Microphone, SystemAudio };

    void refreshDeviceSnapshots(bool fromHotplug);
    void rebuildModels();
    void requestPermission(creator::capture::CaptureDeviceKind kind);
    void handlePermissionResult(
        creator::capture::CaptureDeviceKind kind, std::uint64_t generation,
        creator::core::Result<creator::capture::MediaPermissionStatus> result);
    void startCamera();
    void startMicrophone();
    void startSystemAudio();
    void stopSlot(SlotKind kind, DeviceCaptureState finalState, QString finalMessage);
    void handleStopResult(SlotKind kind, std::uint64_t generation,
                          creator::core::Result<void> result);
    void terminateSlot(SlotKind kind, creator::core::AppError error);
    void drainAudio(SlotKind kind);
    void updateTimer();
    void releaseAll() noexcept;
    [[nodiscard]] const creator::capture::CaptureDeviceInfo* selectedCamera() const;
    [[nodiscard]] const creator::capture::CaptureDeviceInfo* selectedMicrophone() const;
    [[nodiscard]] static bool busy(DeviceCaptureState state) noexcept;

    std::unique_ptr<creator::capture::IDeviceCaptureBackend> backend_;
    std::vector<creator::capture::CaptureDeviceInfo> cameraSnapshot_;
    std::vector<creator::capture::CaptureDeviceInfo> microphoneSnapshot_;
    QVariantList cameraModel_;
    QVariantList microphoneModel_;
    QString selectedCameraId_;
    QString selectedMicrophoneId_;

    std::unique_ptr<creator::capture::IDeviceCaptureSource> cameraSource_;
    std::unique_ptr<creator::capture::IDeviceCaptureSource> microphoneSource_;
    std::unique_ptr<creator::capture::IDeviceCaptureSource> systemAudioSource_;
    std::shared_ptr<creator::capture::LatestVideoFrameMailbox> cameraMailbox_;
    std::shared_ptr<creator::capture::AudioCaptureMailbox> microphoneMailbox_;
    std::shared_ptr<creator::capture::AudioCaptureMailbox> systemAudioMailbox_;

    creator::capture::MediaPermissionStatus cameraPermission_{
        creator::capture::MediaPermissionStatus::Unknown};
    creator::capture::MediaPermissionStatus microphonePermission_{
        creator::capture::MediaPermissionStatus::Unknown};
    DeviceCaptureState cameraState_{DeviceCaptureState::Disabled};
    DeviceCaptureState microphoneState_{DeviceCaptureState::Disabled};
    DeviceCaptureState systemAudioState_{DeviceCaptureState::Ready};
    QString cameraStatus_;
    QString microphoneStatus_;
    QString systemAudioStatus_;
    DeviceCaptureState cameraStopFinalState_{DeviceCaptureState::Ready};
    DeviceCaptureState microphoneStopFinalState_{DeviceCaptureState::Ready};
    DeviceCaptureState systemAudioStopFinalState_{DeviceCaptureState::Ready};
    QString cameraStopFinalMessage_;
    QString microphoneStopFinalMessage_;
    QString systemAudioStopFinalMessage_;
    std::uint64_t cameraGeneration_{0};
    std::uint64_t microphoneGeneration_{0};
    std::uint64_t systemAudioGeneration_{0};
    std::uint64_t cameraPermissionGeneration_{0};
    std::uint64_t microphonePermissionGeneration_{0};

    quint32 cameraWidth_{0};
    quint32 cameraHeight_{0};
    double cameraFps_{0.0};
    creator::capture::AudioLevel microphoneLevel_{};
    creator::capture::AudioLevel systemAudioLevel_{};
    qulonglong microphoneBlocks_{0};
    qulonglong systemAudioBlocks_{0};
    qulonglong microphoneOverruns_{0};
    qulonglong systemAudioOverruns_{0};
    QTimer pollTimer_;
    bool initialized_{false};
};

}  // namespace creator::app
