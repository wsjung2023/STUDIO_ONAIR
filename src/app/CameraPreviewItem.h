#pragma once

#include "app/DeviceCaptureController.h"
#include "app/VideoPreviewItem.h"

#include <QPointer>

namespace creator::app {

class CameraPreviewItem : public VideoPreviewItem {
    Q_OBJECT
    Q_PROPERTY(QObject* captureController READ captureController WRITE setCaptureController
                   NOTIFY captureControllerChanged)

public:
    explicit CameraPreviewItem(QQuickItem* parent = nullptr);

    [[nodiscard]] QObject* captureController() const noexcept { return controller_; }
    void setCaptureController(QObject* controller);
signals:
    void captureControllerChanged();

private:
    void refreshMailbox();
    QPointer<DeviceCaptureController> controller_;
};

}  // namespace creator::app
