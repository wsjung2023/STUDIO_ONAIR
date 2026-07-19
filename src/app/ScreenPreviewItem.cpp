#include "app/ScreenPreviewItem.h"

namespace creator::app {

ScreenPreviewItem::ScreenPreviewItem(QQuickItem* parent)
    : VideoPreviewItem(parent) {}

void ScreenPreviewItem::setCaptureController(QObject* controller) {
    auto* typed = qobject_cast<ScreenCaptureController*>(controller);
    if (controller_ == typed) return;
    if (controller_) disconnect(controller_, nullptr, this, nullptr);
    controller_ = typed;
    if (controller_) {
        connect(controller_, &ScreenCaptureController::statsChanged, this,
                [this] { refreshMailbox(); });
        connect(controller_, &ScreenCaptureController::captureStateChanged, this,
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

void ScreenPreviewItem::refreshMailbox() {
    setMailbox(controller_ ? controller_->previewMailbox() : nullptr);
    update();
}

}  // namespace creator::app
