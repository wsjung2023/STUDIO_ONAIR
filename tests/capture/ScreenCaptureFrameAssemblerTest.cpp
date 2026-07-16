#include "capture/ScreenCaptureFrameAssembler.h"

#include "core/AppError.h"
#include "media/MediaTypes.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

namespace {

using creator::capture::NativeScreenFrame;
using creator::capture::NativeScreenFrameStatus;
using creator::capture::NativeTimestamp;
using creator::capture::ScreenCaptureFrameAssembler;
using creator::core::ErrorCode;
using creator::core::ProjectClock;
using creator::core::TimestampNs;
using creator::media::PixelFormat;

std::shared_ptr<void> handle() { return std::make_shared<int>(42); }

NativeScreenFrame completeFrame(std::int64_t value) {
    return NativeScreenFrame{
        .status = NativeScreenFrameStatus::Complete,
        .timestamp = NativeTimestamp{.value = value, .timescale = 600},
        .width = 1920,
        .height = 1080,
        .pixelFormat = PixelFormat::Bgra8,
        .platformHandle = handle(),
    };
}

TEST(ScreenCaptureFrameAssemblerTest, IgnoresIncompleteFrameWithoutAnchoringTimeline) {
    ScreenCaptureFrameAssembler assembler{
        TimestampNs{ProjectClock::duration{5'000}}};
    auto incomplete = completeFrame(100);
    incomplete.status = NativeScreenFrameStatus::Idle;

    const auto ignored = assembler.assemble(std::move(incomplete));
    const auto firstComplete = assembler.assemble(completeFrame(500));

    ASSERT_TRUE(ignored.hasValue());
    EXPECT_FALSE(ignored.value().has_value());
    ASSERT_TRUE(firstComplete.hasValue());
    ASSERT_TRUE(firstComplete.value().has_value());
    EXPECT_EQ(firstComplete.value()->timestamp.time_since_epoch().count(), 5'000);
}

TEST(ScreenCaptureFrameAssemblerTest, BuildsNeutralFrameAndRetainsNativeHandle) {
    ScreenCaptureFrameAssembler assembler{TimestampNs{ProjectClock::duration{0}}};
    auto nativeHandle = handle();
    auto native = completeFrame(0);
    native.width = 2560;
    native.height = 1440;
    native.platformHandle = nativeHandle;

    const auto result = assembler.assemble(std::move(native));

    ASSERT_TRUE(result.hasValue());
    ASSERT_TRUE(result.value().has_value());
    EXPECT_EQ(result.value()->width, 2560u);
    EXPECT_EQ(result.value()->height, 1440u);
    EXPECT_EQ(result.value()->pixelFormat, PixelFormat::Bgra8);
    EXPECT_EQ(result.value()->platformHandle, nativeHandle);
}

TEST(ScreenCaptureFrameAssemblerTest, RejectsMalformedCompleteFrame) {
    ScreenCaptureFrameAssembler assembler{TimestampNs{ProjectClock::duration{0}}};
    for (int mutation = 0; mutation < 3; ++mutation) {
        auto native = completeFrame(0);
        if (mutation == 0) native.width = 0;
        if (mutation == 1) native.pixelFormat = PixelFormat::Unknown;
        if (mutation == 2) native.platformHandle.reset();

        const auto result = assembler.assemble(std::move(native));

        ASSERT_FALSE(result.hasValue());
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    }
}

TEST(ScreenCaptureFrameAssemblerTest, RejectsBackwardCompleteTimestamp) {
    ScreenCaptureFrameAssembler assembler{TimestampNs{ProjectClock::duration{0}}};
    ASSERT_TRUE(assembler.assemble(completeFrame(100)).hasValue());
    ASSERT_TRUE(assembler.assemble(completeFrame(102)).hasValue());

    const auto result = assembler.assemble(completeFrame(101));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

}  // namespace

