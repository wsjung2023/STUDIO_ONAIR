#include "app/ScreenCaptureController.h"

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
constexpr capture::CaptureConfig kPreviewConfig{};

QString fromUtf8(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

}  // namespace

ScreenCaptureController::ScreenCaptureController(
    std::unique_ptr<capture::IScreenCapturePermission> permission,
    std::unique_ptr<capture::IScreenCaptureDiscovery> discovery,
    std::unique_ptr<capture::IScreenCaptureSourceFactory> sourceFactory, QObject* parent)
    : QObject(parent),
      permission_(std::move(permission)),
      discovery_(std::move(discovery)),
      sourceFactory_(std::move(sourceFactory)) {
    pollTimer_.setInterval(kPollIntervalMs);
    connect(&pollTimer_, &QTimer::timeout, this, &ScreenCaptureController::pollCapture);
}

ScreenCaptureController::~ScreenCaptureController() {
    ++generation_;
    pollTimer_.stop();
    releaseSource();
}

bool ScreenCaptureController::busy() const noexcept {
    return state_ == ScreenCaptureState::CheckingPermission ||
           state_ == ScreenCaptureState::Discovering ||
           state_ == ScreenCaptureState::Starting || state_ == ScreenCaptureState::Stopping;
}

void ScreenCaptureController::initialize() {
    if (state_ != ScreenCaptureState::Idle) {
        return;
    }
    if (!permission_ || !discovery_ || !sourceFactory_) {
        setState(ScreenCaptureState::Error);
        setStatusMessage(tr("Screen capture services are unavailable"));
        return;
    }

    if (permission_->status() == capture::ScreenCapturePermissionStatus::Granted) {
        beginDiscovery();
        return;
    }
    setState(ScreenCaptureState::PermissionRequired);
    setStatusMessage(tr("Screen recording permission is required"));
}

void ScreenCaptureController::requestPermission() {
    if (!permission_ || busy() || previewing()) {
        return;
    }
    const auto generation = ++generation_;
    setState(ScreenCaptureState::CheckingPermission);
    setStatusMessage(tr("Waiting for screen recording permission"));
    QPointer<ScreenCaptureController> self{this};
    permission_->request([self, generation](auto result) mutable {
        auto* context = QCoreApplication::instance();
        if (!context) return;
        QMetaObject::invokeMethod(
            context,
            [self, generation, result = std::move(result)]() mutable {
                if (self) self->handlePermissionResult(generation, std::move(result));
            },
            Qt::QueuedConnection);
    });
}

void ScreenCaptureController::refreshTargets() {
    if (!permission_ || permission_->status() !=
                            capture::ScreenCapturePermissionStatus::Granted ||
        busy() || previewing()) {
        return;
    }
    beginDiscovery();
}

void ScreenCaptureController::beginDiscovery() {
    const auto generation = ++generation_;
    setState(ScreenCaptureState::Discovering);
    setStatusMessage(tr("Finding screens and windows"));
    QPointer<ScreenCaptureController> self{this};
    discovery_->enumerate([self, generation](auto result) mutable {
        auto* context = QCoreApplication::instance();
        if (!context) return;
        QMetaObject::invokeMethod(
            context,
            [self, generation, result = std::move(result)]() mutable {
                if (self) self->handleDiscoveryResult(generation, std::move(result));
            },
            Qt::QueuedConnection);
    });
}

void ScreenCaptureController::handlePermissionResult(
    std::uint64_t generation,
    core::Result<capture::ScreenCapturePermissionStatus> result) {
    if (generation != generation_ || state_ != ScreenCaptureState::CheckingPermission) {
        return;
    }
    if (!result.hasValue()) {
        setState(ScreenCaptureState::Error);
        setStatusMessage(fromUtf8(result.error().message()));
        return;
    }
    if (result.value() != capture::ScreenCapturePermissionStatus::Granted) {
        setState(ScreenCaptureState::PermissionRequired);
        setStatusMessage(tr("Screen recording permission was not granted"));
        return;
    }
    beginDiscovery();
}

void ScreenCaptureController::handleDiscoveryResult(
    std::uint64_t generation,
    core::Result<std::vector<capture::ScreenCaptureTarget>> result) {
    if (generation != generation_ || state_ != ScreenCaptureState::Discovering) {
        return;
    }
    if (!result.hasValue()) {
        setState(ScreenCaptureState::Error);
        setStatusMessage(fromUtf8(result.error().message()));
        return;
    }

    targetSnapshot_ = std::move(result).value();
    rebuildTargetModel();
    if (targetSnapshot_.empty()) {
        selectedTargetId_.clear();
        emit selectedTargetChanged();
        setState(ScreenCaptureState::Error);
        setStatusMessage(tr("No capturable screens or windows were found"));
        return;
    }

    const auto existing = selectedTarget();
    if (!existing) {
        selectedTargetId_ = fromUtf8(targetSnapshot_.front().id().value());
        emit selectedTargetChanged();
    }
    setState(ScreenCaptureState::Ready);
    setStatusMessage(tr("Choose a screen or window to preview"));
}

void ScreenCaptureController::selectTarget(const QString& targetId) {
    if (busy() || previewing()) return;
    const auto found = std::find_if(targetSnapshot_.begin(), targetSnapshot_.end(),
                                    [&targetId](const auto& target) {
                                        return fromUtf8(target.id().value()) == targetId;
                                    });
    if (found == targetSnapshot_.end()) {
        setStatusMessage(tr("The selected capture target is no longer available"));
        return;
    }
    if (selectedTargetId_ != targetId) {
        selectedTargetId_ = targetId;
        emit selectedTargetChanged();
    }
    if (state_ == ScreenCaptureState::Error) setState(ScreenCaptureState::Ready);
    setStatusMessage(tr("Ready to preview"));
}

void ScreenCaptureController::startPreview() {
    if (state_ != ScreenCaptureState::Ready) return;
    const auto* target = selectedTarget();
    if (!target) {
        setStatusMessage(tr("Choose a valid screen or window first"));
        return;
    }

    setState(ScreenCaptureState::Starting);
    setStatusMessage(tr("Starting screen preview"));
    mailbox_ = std::make_shared<capture::LatestVideoFrameMailbox>();
    auto created = sourceFactory_->create(target->id(), mailbox_);
    if (!created.hasValue()) {
        mailbox_.reset();
        setState(ScreenCaptureState::Error);
        setStatusMessage(fromUtf8(created.error().message()));
        return;
    }
    source_ = std::move(created).value();
    auto started = source_->start(kPreviewConfig);
    if (!started.hasValue()) {
        const QString error = fromUtf8(started.error().message());
        releaseSource();
        mailbox_.reset();
        setState(ScreenCaptureState::Error);
        setStatusMessage(error);
        return;
    }

    actualWidth_ = 0;
    actualHeight_ = 0;
    receivedFrames_ = 0;
    droppedFrames_ = 0;
    replacedPreviewFrames_ = 0;
    currentFps_ = 0.0;
    emit statsChanged();
    pollTimer_.start();
    pollCapture();
    if (state_ == ScreenCaptureState::Starting) {
        setStatusMessage(tr("Waiting for the native capture stream"));
    }
}

void ScreenCaptureController::stopPreview() {
    if (state_ != ScreenCaptureState::Starting &&
        state_ != ScreenCaptureState::Previewing) {
        return;
    }
    setState(ScreenCaptureState::Stopping);
    pollTimer_.stop();
    updateStats();
    auto stopped = source_->stop();
    source_.reset();
    mailbox_.reset();
    if (!stopped.hasValue()) {
        setState(ScreenCaptureState::Error);
        setStatusMessage(fromUtf8(stopped.error().message()));
        return;
    }
    setState(targetSnapshot_.empty() ? ScreenCaptureState::Idle : ScreenCaptureState::Ready);
    setStatusMessage(tr("Preview stopped"));
}

void ScreenCaptureController::pollCapture() {
    if (!mailbox_) return;
    updateStats();
    auto error = mailbox_->takeError();
    if (!error) {
        if (state_ == ScreenCaptureState::Starting && mailbox_->takeStarted()) {
            setState(ScreenCaptureState::Previewing);
            setStatusMessage(tr("Previewing"));
        }
        return;
    }

    pollTimer_.stop();
    releaseSource();
    mailbox_.reset();
    setState(ScreenCaptureState::Error);
    setStatusMessage(fromUtf8(error->message()));
}

void ScreenCaptureController::setState(ScreenCaptureState state) {
    if (state_ == state) return;
    state_ = state;
    emit captureStateChanged();
}

void ScreenCaptureController::setStatusMessage(QString message) {
    if (statusMessage_ == message) return;
    statusMessage_ = std::move(message);
    emit statusMessageChanged();
}

void ScreenCaptureController::rebuildTargetModel() {
    QVariantList model;
    model.reserve(static_cast<qsizetype>(targetSnapshot_.size()));
    for (const auto& target : targetSnapshot_) {
        QVariantMap row;
        row.insert(QStringLiteral("id"), fromUtf8(target.id().value()));
        row.insert(QStringLiteral("kind"),
                   target.kind() == capture::ScreenCaptureTargetKind::Display
                       ? QStringLiteral("display")
                       : QStringLiteral("window"));
        row.insert(QStringLiteral("name"), fromUtf8(target.displayName()));
        row.insert(QStringLiteral("application"),
                   target.applicationName() ? fromUtf8(*target.applicationName()) : QString{});
        row.insert(QStringLiteral("width"), target.width());
        row.insert(QStringLiteral("height"), target.height());
        model.push_back(std::move(row));
    }
    targetModel_ = std::move(model);
    emit targetsChanged();
}

void ScreenCaptureController::updateStats() {
    if (!source_ || !mailbox_) return;
    const auto sourceStats = source_->stats();
    const auto mailboxStats = mailbox_->stats();
    const auto received = static_cast<qulonglong>(sourceStats.receivedFrames);
    const auto dropped = static_cast<qulonglong>(sourceStats.droppedFrames);
    const auto replaced = static_cast<qulonglong>(mailboxStats.replacedFrames);
    if (actualWidth_ == mailboxStats.lastWidth && actualHeight_ == mailboxStats.lastHeight &&
        receivedFrames_ == received && droppedFrames_ == dropped &&
        replacedPreviewFrames_ == replaced && currentFps_ == sourceStats.currentFps) {
        return;
    }
    actualWidth_ = mailboxStats.lastWidth;
    actualHeight_ = mailboxStats.lastHeight;
    receivedFrames_ = received;
    droppedFrames_ = dropped;
    replacedPreviewFrames_ = replaced;
    currentFps_ = sourceStats.currentFps;
    emit statsChanged();
}

void ScreenCaptureController::releaseSource() noexcept {
    if (!source_) return;
    static_cast<void>(source_->stop());
    source_.reset();
}

const capture::ScreenCaptureTarget* ScreenCaptureController::selectedTarget() const {
    const auto found = std::find_if(targetSnapshot_.begin(), targetSnapshot_.end(),
                                    [this](const auto& target) {
                                        return fromUtf8(target.id().value()) ==
                                               selectedTargetId_;
                                    });
    return found == targetSnapshot_.end() ? nullptr : &*found;
}

}  // namespace creator::app
