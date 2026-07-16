#include "app/PreviewGeometry.h"

#include <QRectF>
#include <QSize>

#include <gtest/gtest.h>

namespace {

using creator::app::aspectFitRect;

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

}  // namespace

