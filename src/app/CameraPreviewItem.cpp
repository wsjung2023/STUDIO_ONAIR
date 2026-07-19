#include "app/CameraPreviewItem.h"

namespace creator::app {

CameraPreviewItem::CameraPreviewItem(QQuickItem* parent)
    : VideoPreviewItem(parent) {}

void CameraPreviewItem::setCaptureController(QObject* controller) {
    auto* typed = qobject_cast<DeviceCaptureController*>(controller);
    if (controller_ == typed) return;
    if (controller_) disconnect(controller_, nullptr, this, nullptr);
    controller_ = typed;
    if (controller_) {
        connect(controller_, &DeviceCaptureController::statsChanged, this,
                [this] { refreshMailbox(); });
        connect(controller_, &DeviceCaptureController::stateChanged, this,
                [this] { refreshMailbox(); });
        connect(controller_, &QObject::destroyed, this,
                [this] {
                    setMailbox(nullptr);
                    update();
                    emit captureControllerChanged();
                });
    }
    refreshMailbox();
    emit captureControllerChanged();
}

void CameraPreviewItem::refreshMailbox() {
    setMailbox(controller_ ? controller_->cameraPreviewMailbox() : nullptr);
    update();
}

}  // namespace creator::app
