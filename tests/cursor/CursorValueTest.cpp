#include "cursor/CursorButton.h"
#include "cursor/CursorClickEvent.h"
#include "cursor/CursorMoveEvent.h"
#include "cursor/CursorPoint.h"

#include "core/Timebase.h"
#include "domain/Identifiers.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace {

using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::TimestampNs;
using creator::cursor::CursorButton;
using creator::cursor::CursorClickEvent;
using creator::cursor::CursorMoveEvent;
using creator::cursor::CursorPoint;
using creator::cursor::cursorButtonFromString;
using creator::cursor::toString;
using creator::domain::SourceId;

TimestampNs at(std::int64_t ns) {
    return TimestampNs{DurationNs{ns}};
}

SourceId screen() {
    return SourceId::create("screen-1").value();
}

TEST(CursorPointTest, AcceptsCornersAndCenter) {
    EXPECT_TRUE(CursorPoint::create(0.0, 0.0).hasValue());
    EXPECT_TRUE(CursorPoint::create(1.0, 1.0).hasValue());
    const auto center = CursorPoint::create(0.5, 0.5);
    ASSERT_TRUE(center.hasValue());
    EXPECT_DOUBLE_EQ(center.value().x(), 0.5);
    EXPECT_DOUBLE_EQ(center.value().y(), 0.5);
}

TEST(CursorPointTest, RejectsOutOfRange) {
    EXPECT_FALSE(CursorPoint::create(-0.0001, 0.5).hasValue());
    EXPECT_FALSE(CursorPoint::create(0.5, 1.0001).hasValue());
    EXPECT_EQ(CursorPoint::create(2.0, 0.0).error().code(), ErrorCode::InvalidArgument);
}

TEST(CursorPointTest, RejectsNonFinite) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(CursorPoint::create(nan, 0.5).hasValue());
    EXPECT_FALSE(CursorPoint::create(0.5, inf).hasValue());
}

TEST(CursorButtonTest, RoundTripsCanonicalTokens) {
    for (const auto button : {CursorButton::Left, CursorButton::Right, CursorButton::Middle}) {
        const auto parsed = cursorButtonFromString(toString(button));
        ASSERT_TRUE(parsed.hasValue());
        EXPECT_EQ(parsed.value(), button);
    }
    EXPECT_EQ(toString(CursorButton::Left), "left");
    EXPECT_EQ(toString(CursorButton::Right), "right");
    EXPECT_EQ(toString(CursorButton::Middle), "middle");
}

TEST(CursorButtonTest, RejectsUnknownToken) {
    EXPECT_FALSE(cursorButtonFromString("x1").hasValue());
    EXPECT_EQ(cursorButtonFromString("").error().code(), ErrorCode::InvalidArgument);
}

TEST(CursorMoveEventTest, AcceptsNonNegativeTimestamp) {
    const auto point = CursorPoint::create(0.25, 0.75).value();
    const auto event = CursorMoveEvent::create(at(1'000), point, screen());
    ASSERT_TRUE(event.hasValue());
    EXPECT_EQ(event.value().tNs(), at(1'000));
    EXPECT_EQ(event.value().sourceId(), screen());
    EXPECT_EQ(event.value().point(), point);
}

TEST(CursorMoveEventTest, RejectsNegativeTimestamp) {
    const auto point = CursorPoint::create(0.25, 0.75).value();
    const auto event = CursorMoveEvent::create(at(-1), point, screen());
    ASSERT_FALSE(event.hasValue());
    EXPECT_EQ(event.error().code(), ErrorCode::InvalidArgument);
}

TEST(CursorClickEventTest, AcceptsNonNegativeTimestamp) {
    const auto point = CursorPoint::create(0.5, 0.5).value();
    const auto event = CursorClickEvent::create(at(0), point, CursorButton::Right);
    ASSERT_TRUE(event.hasValue());
    EXPECT_EQ(event.value().button(), CursorButton::Right);
}

TEST(CursorClickEventTest, RejectsNegativeTimestamp) {
    const auto point = CursorPoint::create(0.5, 0.5).value();
    const auto event = CursorClickEvent::create(at(-100), point, CursorButton::Left);
    ASSERT_FALSE(event.hasValue());
    EXPECT_EQ(event.error().code(), ErrorCode::InvalidArgument);
}

}  // namespace
