#pragma once

#include "app/AvatarSceneController.h"
#include "app/VideoPreviewItem.h"

#include <QPointer>

namespace creator::app {

/// Qt Quick surface for the live avatar source. Mirrors CameraPreviewItem: it
/// pulls frames from the AvatarSceneController's preview mailbox rather than
/// registering its own sink.
class AvatarPreviewItem : public VideoPreviewItem {
    Q_OBJECT
    Q_PROPERTY(QObject* avatarController READ avatarController WRITE setAvatarController
                   NOTIFY avatarControllerChanged)

public:
    explicit AvatarPreviewItem(QQuickItem* parent = nullptr);

    [[nodiscard]] QObject* avatarController() const noexcept { return controller_; }
    void setAvatarController(QObject* controller);

signals:
    void avatarControllerChanged();

private:
    void refreshMailbox();
    QPointer<AvatarSceneController> controller_;
};

}  // namespace creator::app
