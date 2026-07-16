#include "app/DeviceCaptureController.h"

#include "core/AppError.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QVariantMap>

#include <algorithm>
#include <utility>

namespace creator::app {
namespace {

constexpr int kPollIntervalMs = 16;
constexpr std::size_t kAudioQueueBlocks = 128;

QString fromUtf8(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

template <typename Container>
const capture::CaptureDeviceInfo* selectedDevice(const Container& devices,
                                                 const QString& selectedId) {
    const auto found = std::find_if(devices.begin(), devices.end(),
                                    [&selectedId](const auto& device) {
                                        return fromUtf8(device.id().value()) == selectedId;
                                    });
    return found == devices.end() ? nullptr : &*found;
}

QString defaultDeviceId(const std::vector<capture::CaptureDeviceInfo>& devices) {
    if (devices.empty()) return {};
    const auto found = std::find_if(devices.begin(), devices.end(),
                                    [](const auto& device) { return device.isDefault(); });
    return fromUtf8((found == devices.end() ? devices.front() : *found).id().value());
}

capture::CaptureConfig cameraConfig() {
    capture::CaptureConfig config;
    config.targetWidth = 1920;
    config.targetHeight = 1080;
    config.frameRateNumerator = 30;
    return config;
}

}  // namespace

DeviceCaptureController::DeviceCaptureController(
    std::unique_ptr<capture::IDeviceCaptureBackend> backend, QObject* parent)
    : QObject(parent), backend_(std::move(backend)) {
    pollTimer_.setInterval(kPollIntervalMs);
    connect(&pollTimer_, &QTimer::timeout, this, &DeviceCaptureController::pollCapture);
}

DeviceCaptureController::~DeviceCaptureController() {
    if (backend_) backend_->setDeviceChangeHandler({});
    ++cameraGeneration_;
    ++microphoneGeneration_;
    ++systemAudioGeneration_;
    ++cameraPermissionGeneration_;
    ++microphonePermissionGeneration_;
    pollTimer_.stop();
    releaseAll();
}

bool DeviceCaptureController::cameraPermissionRequired() const noexcept {
    return cameraPermission_ != capture::MediaPermissionStatus::Granted;
}

bool DeviceCaptureController::microphonePermissionRequired() const noexcept {
    return microphonePermission_ != capture::MediaPermissionStatus::Granted;
}

bool DeviceCaptureController::cameraCapturing() const noexcept {
    return cameraState_ == DeviceCaptureState::Capturing;
}

bool DeviceCaptureController::microphoneCapturing() const noexcept {
    return microphoneState_ == DeviceCaptureState::Capturing;
}

bool DeviceCaptureController::systemAudioCapturing() const noexcept {
    return systemAudioState_ == DeviceCaptureState::Capturing;
}

bool DeviceCaptureController::cameraCanStop() const noexcept {
    return cameraSource_ && canStop(cameraState_);
}

bool DeviceCaptureController::microphoneCanStop() const noexcept {
    return microphoneSource_ && canStop(microphoneState_);
}

bool DeviceCaptureController::systemAudioCanStop() const noexcept {
    return canStop(systemAudioState_);
}

bool DeviceCaptureController::cameraBusy() const noexcept { return busy(cameraState_); }
bool DeviceCaptureController::microphoneBusy() const noexcept {
    return busy(microphoneState_);
}
bool DeviceCaptureController::systemAudioBusy() const noexcept {
    return busy(systemAudioState_);
}

bool DeviceCaptureController::busy(DeviceCaptureState state) noexcept {
    return state == DeviceCaptureState::Starting || state == DeviceCaptureState::Stopping;
}

bool DeviceCaptureController::canStop(DeviceCaptureState state) noexcept {
    return state == DeviceCaptureState::Starting || state == DeviceCaptureState::Capturing;
}

void DeviceCaptureController::initialize() {
    if (initialized_) return;
    initialized_ = true;
    if (!backend_) {
        cameraState_ = microphoneState_ = systemAudioState_ = DeviceCaptureState::Error;
        cameraStatus_ = microphoneStatus_ = systemAudioStatus_ =
            tr("Camera and audio services are unavailable");
        emit stateChanged();
        return;
    }

    cameraPermission_ = backend_->permissionStatus(capture::CaptureDeviceKind::Camera);
    microphonePermission_ =
        backend_->permissionStatus(capture::CaptureDeviceKind::Microphone);
    QPointer<DeviceCaptureController> self{this};
    backend_->setDeviceChangeHandler([self] {
        auto* context = QCoreApplication::instance();
        if (!context) return;
        QMetaObject::invokeMethod(
            context, [self] { if (self) self->refreshDeviceSnapshots(true); },
            Qt::QueuedConnection);
    });
    refreshDeviceSnapshots(false);
}

void DeviceCaptureController::refreshDevices() {
    if (!initialized_ || cameraBusy() || microphoneBusy()) return;
    refreshDeviceSnapshots(false);
}

void DeviceCaptureController::refreshDeviceSnapshots(bool fromHotplug) {
    const QString oldCamera = selectedCameraId_;
    const QString oldMicrophone = selectedMicrophoneId_;
    const auto cameras = backend_->devices(capture::CaptureDeviceKind::Camera);
    const auto microphones = backend_->devices(capture::CaptureDeviceKind::Microphone);

    if (cameras.hasValue()) {
        cameraSnapshot_ = cameras.value();
    } else {
        cameraSnapshot_.clear();
        if (!cameraSource_) {
            cameraState_ = DeviceCaptureState::Error;
            cameraStatus_ = fromUtf8(cameras.error().message());
        }
    }
    if (microphones.hasValue()) {
        microphoneSnapshot_ = microphones.value();
    } else {
        microphoneSnapshot_.clear();
        if (!microphoneSource_) {
            microphoneState_ = DeviceCaptureState::Error;
            microphoneStatus_ = fromUtf8(microphones.error().message());
        }
    }

    const bool cameraGone = !oldCamera.isEmpty() && !selectedCamera();
    const bool microphoneGone = !oldMicrophone.isEmpty() && !selectedMicrophone();
    selectedCameraId_ = cameraGone || oldCamera.isEmpty()
                            ? defaultDeviceId(cameraSnapshot_) : oldCamera;
    selectedMicrophoneId_ = microphoneGone || oldMicrophone.isEmpty()
                                ? defaultDeviceId(microphoneSnapshot_) : oldMicrophone;
    rebuildModels();

    if (fromHotplug && cameraGone && cameraSource_) {
        stopSlot(SlotKind::Camera, DeviceCaptureState::Error,
                 tr("Camera disconnected"));
    } else if (!cameraSource_ && cameraState_ != DeviceCaptureState::Error) {
        cameraState_ = cameraPermissionRequired() ? DeviceCaptureState::PermissionRequired
                                                  : DeviceCaptureState::Ready;
        cameraStatus_ = cameraSnapshot_.empty() ? tr("No camera is available")
                                                : tr("Camera ready");
    }

    if (fromHotplug && microphoneGone && microphoneSource_) {
        stopSlot(SlotKind::Microphone, DeviceCaptureState::Error,
                 tr("Microphone disconnected"));
    } else if (!microphoneSource_ && microphoneState_ != DeviceCaptureState::Error) {
        microphoneState_ = microphonePermissionRequired()
                               ? DeviceCaptureState::PermissionRequired
                               : DeviceCaptureState::Ready;
        microphoneStatus_ = microphoneSnapshot_.empty()
                                ? tr("No microphone is available")
                                : tr("Microphone ready");
    }
    emit selectionChanged();
    emit stateChanged();
}

void DeviceCaptureController::rebuildModels() {
    auto build = [](const auto& snapshot) {
        QVariantList model;
        model.reserve(static_cast<qsizetype>(snapshot.size()));
        for (const auto& device : snapshot) {
            QVariantMap row;
            row.insert(QStringLiteral("id"), fromUtf8(device.id().value()));
            row.insert(QStringLiteral("name"), fromUtf8(device.displayName()));
            row.insert(QStringLiteral("default"), device.isDefault());
            model.push_back(std::move(row));
        }
        return model;
    };
    cameraModel_ = build(cameraSnapshot_);
    microphoneModel_ = build(microphoneSnapshot_);
    emit devicesChanged();
}

void DeviceCaptureController::requestCameraPermission() {
    requestPermission(capture::CaptureDeviceKind::Camera);
}

void DeviceCaptureController::requestMicrophonePermission() {
    requestPermission(capture::CaptureDeviceKind::Microphone);
}

void DeviceCaptureController::requestPermission(capture::CaptureDeviceKind kind) {
    if (!backend_) return;
    auto& generation = kind == capture::CaptureDeviceKind::Camera
                           ? cameraPermissionGeneration_ : microphonePermissionGeneration_;
    const auto current = ++generation;
    if (kind == capture::CaptureDeviceKind::Camera) {
        cameraState_ = DeviceCaptureState::Starting;
        cameraStatus_ = tr("Waiting for camera permission");
    } else {
        microphoneState_ = DeviceCaptureState::Starting;
        microphoneStatus_ = tr("Waiting for microphone permission");
    }
    emit stateChanged();
    QPointer<DeviceCaptureController> self{this};
    backend_->requestPermission(kind, [self, kind, current](auto result) mutable {
        auto* context = QCoreApplication::instance();
        if (!context) return;
        QMetaObject::invokeMethod(
            context, [self, kind, current, result = std::move(result)]() mutable {
                if (self) self->handlePermissionResult(kind, current, std::move(result));
            }, Qt::QueuedConnection);
    });
}

void DeviceCaptureController::handlePermissionResult(
    capture::CaptureDeviceKind kind, std::uint64_t generation,
    core::Result<capture::MediaPermissionStatus> result) {
    auto& expected = kind == capture::CaptureDeviceKind::Camera
                         ? cameraPermissionGeneration_ : microphonePermissionGeneration_;
    if (generation != expected) return;
    if (!result.hasValue()) {
        if (kind == capture::CaptureDeviceKind::Camera) {
            cameraState_ = DeviceCaptureState::Error;
            cameraStatus_ = fromUtf8(result.error().message());
        } else {
            microphoneState_ = DeviceCaptureState::Error;
            microphoneStatus_ = fromUtf8(result.error().message());
        }
        emit stateChanged();
        return;
    }
    if (kind == capture::CaptureDeviceKind::Camera) cameraPermission_ = result.value();
    else microphonePermission_ = result.value();
    if (result.value() == capture::MediaPermissionStatus::Granted) {
        refreshDeviceSnapshots(false);
    } else {
        if (kind == capture::CaptureDeviceKind::Camera) {
            cameraState_ = DeviceCaptureState::PermissionRequired;
            cameraStatus_ = tr("Camera permission was not granted");
        } else {
            microphoneState_ = DeviceCaptureState::PermissionRequired;
            microphoneStatus_ = tr("Microphone permission was not granted");
        }
        emit stateChanged();
    }
}

void DeviceCaptureController::selectCamera(const QString& deviceId) {
    if (cameraSource_ || cameraBusy()) return;
    if (!selectedDevice(cameraSnapshot_, deviceId)) {
        cameraStatus_ = tr("Selected camera is no longer available");
        emit stateChanged();
        return;
    }
    selectedCameraId_ = deviceId;
    emit selectionChanged();
}

void DeviceCaptureController::selectMicrophone(const QString& deviceId) {
    if (microphoneSource_ || microphoneBusy()) return;
    if (!selectedDevice(microphoneSnapshot_, deviceId)) {
        microphoneStatus_ = tr("Selected microphone is no longer available");
        emit stateChanged();
        return;
    }
    selectedMicrophoneId_ = deviceId;
    emit selectionChanged();
}

void DeviceCaptureController::setCameraEnabled(bool enabled) {
    if (enabled) startCamera();
    else stopSlot(SlotKind::Camera, DeviceCaptureState::Ready, tr("Camera stopped"));
}

void DeviceCaptureController::setMicrophoneEnabled(bool enabled) {
    if (enabled) startMicrophone();
    else stopSlot(SlotKind::Microphone, DeviceCaptureState::Ready,
                  tr("Microphone stopped"));
}

void DeviceCaptureController::setSystemAudioEnabled(bool enabled) {
    if (enabled) startSystemAudio();
    else stopSlot(SlotKind::SystemAudio, DeviceCaptureState::Ready,
                  tr("System audio stopped"));
}

void DeviceCaptureController::startCamera() {
    if (cameraSource_ || cameraBusy()) return;
    const auto* device = selectedCamera();
    if (cameraPermissionRequired() || !device) {
        cameraState_ = cameraPermissionRequired() ? DeviceCaptureState::PermissionRequired
                                                  : DeviceCaptureState::Error;
        cameraStatus_ = cameraPermissionRequired() ? tr("Camera permission is required")
                                                   : tr("Choose an available camera");
        emit stateChanged();
        return;
    }
    cameraState_ = DeviceCaptureState::Starting;
    cameraStatus_ = tr("Starting camera");
    cameraWidth_ = 0;
    cameraHeight_ = 0;
    cameraFps_ = 0.0;
    ++cameraGeneration_;
    cameraMailbox_ = std::make_shared<capture::LatestVideoFrameMailbox>();
    auto created = backend_->createCamera(device->id(), cameraMailbox_);
    if (!created.hasValue()) {
        cameraMailbox_.reset();
        cameraState_ = DeviceCaptureState::Error;
        cameraStatus_ = fromUtf8(created.error().message());
        emit stateChanged();
        return;
    }
    cameraSource_ = std::move(created).value();
    auto started = cameraSource_->start(cameraConfig());
    if (!started.hasValue()) {
        const auto message = fromUtf8(started.error().message());
        static_cast<void>(cameraSource_->stop());
        cameraSource_.reset();
        cameraMailbox_.reset();
        cameraState_ = DeviceCaptureState::Error;
        cameraStatus_ = message;
    }
    updateTimer();
    emit stateChanged();
}

void DeviceCaptureController::startMicrophone() {
    if (microphoneSource_ || microphoneBusy()) return;
    const auto* device = selectedMicrophone();
    if (microphonePermissionRequired() || !device) {
        microphoneState_ = microphonePermissionRequired()
                               ? DeviceCaptureState::PermissionRequired
                               : DeviceCaptureState::Error;
        microphoneStatus_ = microphonePermissionRequired()
                                ? tr("Microphone permission is required")
                                : tr("Choose an available microphone");
        emit stateChanged();
        return;
    }
    microphoneState_ = DeviceCaptureState::Starting;
    microphoneStatus_ = tr("Starting microphone");
    microphoneLevel_ = {};
    microphoneBlocks_ = 0;
    microphoneOverruns_ = 0;
    ++microphoneGeneration_;
    microphoneMailbox_ = std::make_shared<capture::AudioCaptureMailbox>(kAudioQueueBlocks);
    auto created = backend_->createMicrophone(device->id(), microphoneMailbox_);
    if (!created.hasValue()) {
        microphoneMailbox_.reset();
        microphoneState_ = DeviceCaptureState::Error;
        microphoneStatus_ = fromUtf8(created.error().message());
        emit stateChanged();
        return;
    }
    microphoneSource_ = std::move(created).value();
    auto started = microphoneSource_->start({});
    if (!started.hasValue()) {
        const auto message = fromUtf8(started.error().message());
        static_cast<void>(microphoneSource_->stop());
        microphoneSource_.reset();
        microphoneMailbox_.reset();
        microphoneState_ = DeviceCaptureState::Error;
        microphoneStatus_ = message;
    }
    updateTimer();
    emit stateChanged();
}

void DeviceCaptureController::startSystemAudio() {
    if (systemAudioSource_ || systemAudioBusy()) return;
    systemAudioState_ = DeviceCaptureState::Starting;
    systemAudioStatus_ = tr("Starting system audio");
    systemAudioLevel_ = {};
    systemAudioBlocks_ = 0;
    systemAudioOverruns_ = 0;
    ++systemAudioGeneration_;
    systemAudioMailbox_ = std::make_shared<capture::AudioCaptureMailbox>(kAudioQueueBlocks);
    auto created = backend_->createSystemAudio(systemAudioMailbox_);
    if (!created.hasValue()) {
        systemAudioMailbox_.reset();
        systemAudioState_ = DeviceCaptureState::Error;
        systemAudioStatus_ = fromUtf8(created.error().message());
        emit stateChanged();
        return;
    }
    systemAudioSource_ = std::move(created).value();
    auto started = systemAudioSource_->start({});
    if (!started.hasValue()) {
        const auto message = fromUtf8(started.error().message());
        static_cast<void>(systemAudioSource_->stop());
        systemAudioSource_.reset();
        systemAudioMailbox_.reset();
        systemAudioState_ = DeviceCaptureState::Error;
        systemAudioStatus_ = message;
    }
    updateTimer();
    emit stateChanged();
}

void DeviceCaptureController::stopSlot(SlotKind kind, DeviceCaptureState finalState,
                                       QString finalMessage) {
    auto* source = kind == SlotKind::Camera ? cameraSource_.get()
                   : kind == SlotKind::Microphone ? microphoneSource_.get()
                                                  : systemAudioSource_.get();
    if (!source) return;
    auto& state = kind == SlotKind::Camera ? cameraState_
                  : kind == SlotKind::Microphone ? microphoneState_
                                                 : systemAudioState_;
    if (state == DeviceCaptureState::Stopping) return;
    auto& generation = kind == SlotKind::Camera ? cameraGeneration_
                       : kind == SlotKind::Microphone ? microphoneGeneration_
                                                      : systemAudioGeneration_;
    auto& pendingState = kind == SlotKind::Camera ? cameraStopFinalState_
                         : kind == SlotKind::Microphone ? microphoneStopFinalState_
                                                        : systemAudioStopFinalState_;
    auto& pendingMessage = kind == SlotKind::Camera ? cameraStopFinalMessage_
                           : kind == SlotKind::Microphone ? microphoneStopFinalMessage_
                                                          : systemAudioStopFinalMessage_;
    pendingState = finalState;
    pendingMessage = std::move(finalMessage);
    state = DeviceCaptureState::Stopping;
    const auto current = ++generation;
    emit stateChanged();
    QPointer<DeviceCaptureController> self{this};
    source->stopAsync([self, kind, current](auto result) mutable {
        auto* context = QCoreApplication::instance();
        if (!context) return;
        QMetaObject::invokeMethod(
            context, [self, kind, current, result = std::move(result)]() mutable {
                if (self) self->handleStopResult(kind, current, std::move(result));
            }, Qt::QueuedConnection);
    });
}

void DeviceCaptureController::handleStopResult(SlotKind kind, std::uint64_t generation,
                                               core::Result<void> result) {
    auto& expected = kind == SlotKind::Camera ? cameraGeneration_
                     : kind == SlotKind::Microphone ? microphoneGeneration_
                                                    : systemAudioGeneration_;
    auto& state = kind == SlotKind::Camera ? cameraState_
                  : kind == SlotKind::Microphone ? microphoneState_
                                                 : systemAudioState_;
    if (generation != expected || state != DeviceCaptureState::Stopping) return;
    auto& source = kind == SlotKind::Camera ? cameraSource_
                   : kind == SlotKind::Microphone ? microphoneSource_
                                                  : systemAudioSource_;
    source.reset();
    if (kind == SlotKind::Camera) cameraMailbox_.reset();
    else if (kind == SlotKind::Microphone) microphoneMailbox_.reset();
    else systemAudioMailbox_.reset();
    if (!result.hasValue()) {
        state = DeviceCaptureState::Error;
        const auto message = fromUtf8(result.error().message());
        if (kind == SlotKind::Camera) cameraStatus_ = message;
        else if (kind == SlotKind::Microphone) microphoneStatus_ = message;
        else systemAudioStatus_ = message;
    } else {
        const auto finalState = kind == SlotKind::Camera ? cameraStopFinalState_
                                : kind == SlotKind::Microphone ? microphoneStopFinalState_
                                                               : systemAudioStopFinalState_;
        const auto message = kind == SlotKind::Camera ? cameraStopFinalMessage_
                             : kind == SlotKind::Microphone ? microphoneStopFinalMessage_
                                                            : systemAudioStopFinalMessage_;
        state = finalState;
        if (kind == SlotKind::Camera) cameraStatus_ = message;
        else if (kind == SlotKind::Microphone) microphoneStatus_ = message;
        else systemAudioStatus_ = message;
    }
    updateTimer();
    emit stateChanged();
}

void DeviceCaptureController::pollCapture() {
    if (cameraSource_ && cameraMailbox_ && cameraState_ != DeviceCaptureState::Stopping) {
        if (cameraState_ == DeviceCaptureState::Starting && cameraMailbox_->takeStarted()) {
            cameraState_ = DeviceCaptureState::Capturing;
            cameraStatus_ = tr("Camera capturing");
            emit stateChanged();
        }
        const auto sourceStats = cameraSource_->stats();
        const auto mailboxStats = cameraMailbox_->stats();
        cameraWidth_ = mailboxStats.lastWidth;
        cameraHeight_ = mailboxStats.lastHeight;
        cameraFps_ = sourceStats.currentFps;
        if (auto error = cameraMailbox_->takeError()) {
            terminateSlot(SlotKind::Camera, std::move(*error));
        }
    }
    drainAudio(SlotKind::Microphone);
    drainAudio(SlotKind::SystemAudio);
    emit statsChanged();
    updateTimer();
}

void DeviceCaptureController::drainAudio(SlotKind kind) {
    auto& source = kind == SlotKind::Microphone ? microphoneSource_ : systemAudioSource_;
    auto& mailbox = kind == SlotKind::Microphone ? microphoneMailbox_ : systemAudioMailbox_;
    auto& state = kind == SlotKind::Microphone ? microphoneState_ : systemAudioState_;
    if (!source || !mailbox || state == DeviceCaptureState::Stopping) return;
    if (state == DeviceCaptureState::Starting && mailbox->takeStarted()) {
        state = DeviceCaptureState::Capturing;
        if (kind == SlotKind::Microphone) microphoneStatus_ = tr("Microphone capturing");
        else systemAudioStatus_ = tr("System audio capturing");
        emit stateChanged();
    }
    while (auto block = mailbox->tryPop()) {
        auto level = capture::AudioLevelMeter::measure(*block);
        if (!level.hasValue()) {
            terminateSlot(kind, level.error());
            return;
        }
        if (kind == SlotKind::Microphone) microphoneLevel_ = level.value();
        else systemAudioLevel_ = level.value();
    }
    const auto stats = mailbox->stats();
    if (kind == SlotKind::Microphone) {
        microphoneBlocks_ = static_cast<qulonglong>(stats.receivedBlocks);
        microphoneOverruns_ = static_cast<qulonglong>(stats.overruns);
    } else {
        systemAudioBlocks_ = static_cast<qulonglong>(stats.receivedBlocks);
        systemAudioOverruns_ = static_cast<qulonglong>(stats.overruns);
    }
    if (auto error = mailbox->takeError()) terminateSlot(kind, std::move(*error));
}

void DeviceCaptureController::terminateSlot(SlotKind kind, core::AppError error) {
    auto& source = kind == SlotKind::Camera ? cameraSource_
                   : kind == SlotKind::Microphone ? microphoneSource_
                                                  : systemAudioSource_;
    if (source) static_cast<void>(source->stop());
    source.reset();
    if (kind == SlotKind::Camera) cameraMailbox_.reset();
    else if (kind == SlotKind::Microphone) microphoneMailbox_.reset();
    else systemAudioMailbox_.reset();
    const auto message = fromUtf8(error.message());
    if (kind == SlotKind::Camera) {
        cameraState_ = DeviceCaptureState::Error;
        cameraStatus_ = message;
    } else if (kind == SlotKind::Microphone) {
        microphoneState_ = DeviceCaptureState::Error;
        microphoneStatus_ = message;
    } else {
        systemAudioState_ = DeviceCaptureState::Error;
        systemAudioStatus_ = message;
    }
    emit stateChanged();
}

void DeviceCaptureController::updateTimer() {
    const bool needsPolling = cameraSource_ || microphoneSource_ || systemAudioSource_;
    if (needsPolling && !pollTimer_.isActive()) pollTimer_.start();
    if (!needsPolling) pollTimer_.stop();
}

void DeviceCaptureController::releaseAll() noexcept {
    for (auto* source : {cameraSource_.get(), microphoneSource_.get(),
                         systemAudioSource_.get()}) {
        if (source) static_cast<void>(source->stop());
    }
    cameraSource_.reset();
    microphoneSource_.reset();
    systemAudioSource_.reset();
    cameraMailbox_.reset();
    microphoneMailbox_.reset();
    systemAudioMailbox_.reset();
}

const capture::CaptureDeviceInfo* DeviceCaptureController::selectedCamera() const {
    return selectedDevice(cameraSnapshot_, selectedCameraId_);
}

const capture::CaptureDeviceInfo* DeviceCaptureController::selectedMicrophone() const {
    return selectedDevice(microphoneSnapshot_, selectedMicrophoneId_);
}

}  // namespace creator::app
