#include "app/PreviewGeometry.h"

#include <algorithm>

namespace creator::app {

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

