#pragma once

#include "app/ScreenCaptureController.h"
#include "app/VideoPreviewItem.h"

#include <QPointer>
namespace creator::app {

/// Qt Quick surface that consumes the latest capture frame on the render thread.
class ScreenPreviewItem : public VideoPreviewItem {
    Q_OBJECT
    Q_PROPERTY(QObject* captureController READ captureController WRITE setCaptureController
                   NOTIFY captureControllerChanged)

public:
    explicit ScreenPreviewItem(QQuickItem* parent = nullptr);

    [[nodiscard]] QObject* captureController() const noexcept { return controller_; }
    void setCaptureController(QObject* controller);
signals:
    void captureControllerChanged();
private:
    void refreshMailbox();
    QPointer<ScreenCaptureController> controller_;
};

}  // namespace creator::app
