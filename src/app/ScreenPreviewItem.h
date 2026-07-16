#pragma once

#include "app/ScreenCaptureController.h"
#include "capture/LatestVideoFrameMailbox.h"

#include <QPointer>
#include <QQuickItem>
#include <QString>

#include <memory>

namespace creator::app {

/// Qt Quick surface that consumes the latest capture frame on the render thread.
class ScreenPreviewItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QObject* captureController READ captureController WRITE setCaptureController
                   NOTIFY captureControllerChanged)
    Q_PROPERTY(bool frameVisible READ frameVisible NOTIFY renderStateChanged)
    Q_PROPERTY(QString rendererStatus READ rendererStatus NOTIFY renderStateChanged)

public:
    explicit ScreenPreviewItem(QQuickItem* parent = nullptr);

    [[nodiscard]] QObject* captureController() const noexcept { return controller_; }
    void setCaptureController(QObject* controller);
    [[nodiscard]] bool frameVisible() const noexcept { return frameVisible_; }
    [[nodiscard]] QString rendererStatus() const { return rendererStatus_; }

    [[nodiscard]] std::shared_ptr<creator::capture::LatestVideoFrameMailbox>
    mailbox() const noexcept;

    /// Render-thread helper used by the platform node. The property mutation is
    /// queued back to the GUI thread and coalesced by value.
    void postRenderState(bool frameVisible, QString status);

signals:
    void captureControllerChanged();
    void renderStateChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;

private:
    void applyRenderState(bool frameVisible, QString status);

    QPointer<ScreenCaptureController> controller_;
    bool frameVisible_{false};
    QString rendererStatus_;
};

}  // namespace creator::app
