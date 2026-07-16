#include "app/PreviewGeometry.h"

#include <algorithm>

namespace creator::app {

PreviewFrameGeometry previewFrameGeometry(const media::VideoFrame& frame) noexcept {
    const auto visible = frame.visibleRect.width != 0 && frame.visibleRect.height != 0
                             ? frame.visibleRect
                             : media::PixelRect{.width = frame.width, .height = frame.height};
    const auto contentWidth = frame.contentWidth != 0 ? frame.contentWidth : visible.width;
    const auto contentHeight = frame.contentHeight != 0 ? frame.contentHeight : visible.height;
    return PreviewFrameGeometry{
        .contentSize = QSize{static_cast<int>(contentWidth),
                             static_cast<int>(contentHeight)},
        .sourceRect = QRectF{static_cast<qreal>(visible.x),
                             static_cast<qreal>(visible.y),
                             static_cast<qreal>(visible.width),
                             static_cast<qreal>(visible.height)},
    };
}

QRectF aspectFitRect(const QSize& contentSize, const QRectF& bounds) noexcept {
    if (contentSize.width() <= 0 || contentSize.height() <= 0 || bounds.width() <= 0.0 ||
        bounds.height() <= 0.0) {
        return {};
    }
    const qreal scale =
        std::min(bounds.width() / static_cast<qreal>(contentSize.width()),
                 bounds.height() / static_cast<qreal>(contentSize.height()));
    const QSizeF fitted{static_cast<qreal>(contentSize.width()) * scale,
                        static_cast<qreal>(contentSize.height()) * scale};
    return QRectF{bounds.center().x() - fitted.width() / 2.0,
                  bounds.center().y() - fitted.height() / 2.0, fitted.width(),
                  fitted.height()};
}

}  // namespace creator::app
