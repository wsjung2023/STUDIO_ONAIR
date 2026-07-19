#include "app/EditorPreviewItem.h"

#include <QColor>
#include <QFont>
#include <QPainter>
#include <QRectF>
#include <QSizeF>

#include <utility>

namespace creator::app {

EditorPreviewItem::EditorPreviewItem(QQuickItem* parent)
    : QQuickPaintedItem(parent) {
    setAntialiasing(false);
    setOpaquePainting(true);
}

void EditorPreviewItem::setFrame(QImage frame) {
    if (frame.cacheKey() == frame_.cacheKey() && frame.size() == frame_.size()) {
        return;
    }
    frame_ = std::move(frame);
    emit frameChanged();
    update();
}

void EditorPreviewItem::setStale(bool stale) {
    if (stale_ == stale) return;
    stale_ = stale;
    emit stateChanged();
    update();
}

void EditorPreviewItem::setStatusText(QString statusText) {
    if (statusText_ == statusText) return;
    statusText_ = std::move(statusText);
    emit stateChanged();
    update();
}

void EditorPreviewItem::paint(QPainter* painter) {
    const QRectF bounds{0.0, 0.0, width(), height()};
    painter->fillRect(bounds, QColor{13, 15, 18});

    if (!frame_.isNull()) {
        QSizeF targetSize = frame_.size();
        targetSize.scale(bounds.size(), Qt::KeepAspectRatio);
        const QRectF target{
            (bounds.width() - targetSize.width()) / 2.0,
            (bounds.height() - targetSize.height()) / 2.0,
            targetSize.width(), targetSize.height()};
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter->drawImage(target, frame_);
    }

    if (stale_ || frame_.isNull()) {
        painter->fillRect(bounds, QColor{0, 0, 0, frame_.isNull() ? 80 : 145});
        painter->setPen(QColor{stale_ ? 255 : 225, stale_ ? 190 : 225,
                              stale_ ? 102 : 225});
        QFont font = painter->font();
        font.setPixelSize(18);
        painter->setFont(font);
        const QString message = statusText_.isEmpty()
                                    ? tr("Preview unavailable")
                                    : statusText_;
        painter->drawText(bounds.adjusted(24, 24, -24, -24),
                          Qt::AlignCenter | Qt::TextWordWrap, message);
    }
}

}  // namespace creator::app
