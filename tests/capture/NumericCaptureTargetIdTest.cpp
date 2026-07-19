#include "capture/NumericCaptureTargetId.h"

#include "capture/ScreenCaptureTypes.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {

using creator::capture::makeNumericCaptureTargetId;
using creator::capture::ScreenCaptureTargetKind;

TEST(NumericCaptureTargetIdTest, NamespacesDisplayAndWindowIds) {
    const auto display = makeNumericCaptureTargetId(ScreenCaptureTargetKind::Display, 42);
    const auto window = makeNumericCaptureTargetId(ScreenCaptureTargetKind::Window, 42);

    ASSERT_TRUE(display.hasValue());
    ASSERT_TRUE(window.hasValue());
    EXPECT_EQ(display.value().value(), "display:42");
    EXPECT_EQ(window.value().value(), "window:42");
    EXPECT_NE(display.value(), window.value());
}

TEST(NumericCaptureTargetIdTest, PreservesFullUnsignedNativeRange) {
    const auto result = makeNumericCaptureTargetId(
        ScreenCaptureTargetKind::Display, std::numeric_limits<std::uint64_t>::max());

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().value(), "display:18446744073709551615");
}

}  // namespace

