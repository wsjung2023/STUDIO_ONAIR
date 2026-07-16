#include "app/PreviewGeometry.h"

#include <QRectF>
#include <QSize>

#include <gtest/gtest.h>

namespace {

using creator::app::aspectFitRect;
using creator::app::previewFrameGeometry;

TEST(PreviewGeometryTest, CentersWideFrameInsideTallViewport) {
    const QRectF fitted = aspectFitRect(QSize{1920, 1080}, QRectF{0, 0, 800, 600});

    EXPECT_DOUBLE_EQ(fitted.x(), 0.0);
    EXPECT_DOUBLE_EQ(fitted.y(), 75.0);
    EXPECT_DOUBLE_EQ(fitted.width(), 800.0);
    EXPECT_DOUBLE_EQ(fitted.height(), 450.0);
}

TEST(PreviewGeometryTest, CentersTallFrameInsideWideViewport) {
    const QRectF fitted = aspectFitRect(QSize{1080, 1920}, QRectF{0, 0, 800, 600});

    EXPECT_DOUBLE_EQ(fitted.x(), 231.25);
    EXPECT_DOUBLE_EQ(fitted.y(), 0.0);
    EXPECT_DOUBLE_EQ(fitted.width(), 337.5);
    EXPECT_DOUBLE_EQ(fitted.height(), 600.0);
}

TEST(PreviewGeometryTest, RejectsEmptyContentOrViewport) {
    EXPECT_TRUE(aspectFitRect(QSize{}, QRectF{0, 0, 800, 600}).isEmpty());
    EXPECT_TRUE(aspectFitRect(QSize{1920, 1080}, QRectF{}).isEmpty());
}

TEST(PreviewGeometryTest, UsesVisibleCropAndNativeContentSize) {
    creator::media::VideoFrame frame{
        .width = 1920,
        .height = 1080,
        .visibleRect = {.x = 160, .y = 90, .width = 1600, .height = 900},
        .contentWidth = 2560,
        .contentHeight = 1440,
    };

    const auto geometry = previewFrameGeometry(frame);

    EXPECT_EQ(geometry.contentSize, QSize(2560, 1440));
    EXPECT_EQ(geometry.sourceRect, QRectF(160, 90, 1600, 900));
}

}  // namespace
