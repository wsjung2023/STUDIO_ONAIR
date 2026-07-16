#include "app/ScreenPreviewItem.h"

#include <QMetaObject>

#include <utility>

namespace creator::app {

ScreenPreviewItem::ScreenPreviewItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

void ScreenPreviewItem::setCaptureController(QObject* controller) {
    auto* typed = qobject_cast<ScreenCaptureController*>(controller);
    if (controller_ == typed) return;
    if (controller_) disconnect(controller_, nullptr, this, nullptr);
    controller_ = typed;
    if (controller_) {
        connect(controller_, &ScreenCaptureController::statsChanged, this,
                [this] { update(); });
        connect(controller_, &ScreenCaptureController::captureStateChanged, this,
                [this] { update(); });
    }
    emit captureControllerChanged();
    update();
}

std::shared_ptr<capture::LatestVideoFrameMailbox> ScreenPreviewItem::mailbox() const noexcept {
    return controller_ ? controller_->previewMailbox() : nullptr;
}

void ScreenPreviewItem::postRenderState(bool frameVisible, QString status) {
    QPointer<ScreenPreviewItem> self{this};
    QMetaObject::invokeMethod(
        this,
        [self, frameVisible, status = std::move(status)]() mutable {
            if (self) self->applyRenderState(frameVisible, std::move(status));
        },
        Qt::QueuedConnection);
}

void ScreenPreviewItem::applyRenderState(bool frameVisible, QString status) {
    if (frameVisible_ == frameVisible && rendererStatus_ == status) return;
    frameVisible_ = frameVisible;
    rendererStatus_ = std::move(status);
    emit renderStateChanged();
}

}  // namespace creator::app

