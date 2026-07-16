#pragma once

#include <QRectF>
#include <QSize>

namespace creator::app {

/// Aspect-fits pixel content inside an item rectangle without cropping.
[[nodiscard]] QRectF aspectFitRect(const QSize& contentSize, const QRectF& bounds) noexcept;

}  // namespace creator::app

