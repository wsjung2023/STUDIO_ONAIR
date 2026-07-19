#pragma once

#include "media/MediaTypes.h"

#include <QRectF>
#include <QSize>

namespace creator::app {

struct PreviewFrameGeometry final {
    QSize contentSize;
    QRectF sourceRect;
};

/// Converts neutral pixel metadata into the crop and aspect used by Qt Quick.
[[nodiscard]] PreviewFrameGeometry previewFrameGeometry(
    const creator::media::VideoFrame& frame) noexcept;

/// Aspect-fits pixel content inside an item rectangle without cropping.
[[nodiscard]] QRectF aspectFitRect(const QSize& contentSize, const QRectF& bounds) noexcept;

}  // namespace creator::app
