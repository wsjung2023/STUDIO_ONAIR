#include "app/AvatarPreviewItem.h"

namespace creator::app {

AvatarPreviewItem::AvatarPreviewItem(QQuickItem* parent)
    : VideoPreviewItem(parent) {}

void AvatarPreviewItem::setAvatarController(QObject* controller) {
    auto* typed = qobject_cast<AvatarSceneController*>(controller);
    if (controller_ == typed) return;
    if (controller_) disconnect(controller_, nullptr, this, nullptr);
    controller_ = typed;
    if (controller_) {
        connect(controller_, &AvatarSceneController::stateChanged, this,
                [this] { refreshMailbox(); });
        // Repaint on every produced frame so the live avatar actually moves;
        // stateChanged alone fires far too rarely for ~30 fps animation.
        connect(controller_, &AvatarSceneController::previewFrameReady, this,
                [this] { update(); });
        connect(controller_, &QObject::destroyed, this, [this] {
            setMailbox(nullptr);
            update();
            emit avatarControllerChanged();
        });
    }
    refreshMailbox();
    emit avatarControllerChanged();
}

void AvatarPreviewItem::refreshMailbox() {
    setMailbox(controller_ ? controller_->avatarPreviewMailbox() : nullptr);
    update();
}

}  // namespace creator::app
