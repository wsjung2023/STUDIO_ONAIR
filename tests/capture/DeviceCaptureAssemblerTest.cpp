#include "capture/AudioCaptureBlockAssembler.h"
#include "capture/CameraCaptureFrameAssembler.h"

#include "core/Timebase.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <vector>

namespace {

using creator::capture::AudioCaptureBlockAssembler;
using creator::capture::CameraCaptureFrameAssembler;
using creator::capture::NativeAudioBlock;
using creator::capture::NativeCameraFrame;
using creator::capture::NativeTimestamp;
using creator::core::ErrorCode;
using creator::core::ProjectClock;
using creator::core::TimestampNs;
using creator::media::PixelFormat;

std::shared_ptr<void> videoHandle() { return std::make_shared<int>(7); }

NativeCameraFrame cameraFrame(std::int64_t value, std::int32_t timescale = 60) {
    return NativeCameraFrame{
        .timestamp = NativeTimestamp{value, timescale},
        .width = 1280,
        .height = 720,
        .pixelFormat = PixelFormat::Bgra8,
        .platformHandle = videoHandle(),
    };
}

NativeAudioBlock audioBlock(std::int64_t value, std::vector<float> values,
                            std::uint32_t channels = 2) {
    auto samples = std::shared_ptr<float[]>(new float[values.size()],
                                            std::default_delete<float[]>{});
    for (std::size_t index = 0; index < values.size(); ++index) {
        samples[index] = values[index];
    }
    return NativeAudioBlock{
        .timestamp = NativeTimestamp{value, 48'000},
        .sampleRate = 48'000,
        .channels = channels,
        .frameCount = static_cast<std::uint32_t>(values.size() / channels),
        .sampleCount = values.size(),
        .samples = std::move(samples),
    };
}

TEST(CameraCaptureFrameAssemblerTest, AnchorsFirstValidFrameAndMapsExactCadence) {
    CameraCaptureFrameAssembler assembler{
        TimestampNs{ProjectClock::duration{1'000'000'000}}};

    const auto first = assembler.assemble(cameraFrame(120));
    const auto second = assembler.assemble(cameraFrame(121));

    ASSERT_TRUE(first.hasValue());
    ASSERT_TRUE(second.hasValue());
    EXPECT_EQ(first.value().timestamp.time_since_epoch().count(), 1'000'000'000);
    EXPECT_EQ(second.value().timestamp.time_since_epoch().count(), 1'016'666'666);
}

TEST(CameraCaptureFrameAssemblerTest, InvalidFrameDoesNotSampleProjectAnchor) {
    int anchorCalls = 0;
    CameraCaptureFrameAssembler assembler{[&anchorCalls] {
        ++anchorCalls;
        return TimestampNs{ProjectClock::duration{500}};
    }};
    auto invalid = cameraFrame(1);
    invalid.width = 0;
    auto invalidTimestamp = cameraFrame(1);
    invalidTimestamp.timestamp.timescale = 0;

    EXPECT_FALSE(assembler.assemble(std::move(invalid)).hasValue());
    EXPECT_FALSE(assembler.assemble(std::move(invalidTimestamp)).hasValue());
    EXPECT_EQ(anchorCalls, 0);
    const auto valid = assembler.assemble(cameraFrame(20));
    ASSERT_TRUE(valid.hasValue());
    EXPECT_EQ(valid.value().timestamp.time_since_epoch().count(), 500);
    EXPECT_EQ(anchorCalls, 1);
}

TEST(CameraCaptureFrameAssemblerTest, ProducesFullSurfaceNeutralFrame) {
    CameraCaptureFrameAssembler assembler{TimestampNs{ProjectClock::duration{0}}};
    auto handle = videoHandle();
    auto native = cameraFrame(0);
    native.platformHandle = handle;

    const auto result = assembler.assemble(std::move(native));

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().width, 1280u);
    EXPECT_EQ(result.value().height, 720u);
    EXPECT_EQ(result.value().visibleRect,
              (creator::media::PixelRect{0, 0, 1280, 720}));
    EXPECT_EQ(result.value().contentWidth, 1280u);
    EXPECT_EQ(result.value().contentHeight, 720u);
    EXPECT_EQ(result.value().platformHandle, handle);
}

TEST(CameraCaptureFrameAssemblerTest, RejectsMalformedGeometryFormatAndHandle) {
    CameraCaptureFrameAssembler assembler{TimestampNs{ProjectClock::duration{0}}};
    for (int mutation = 0; mutation < 4; ++mutation) {
        auto native = cameraFrame(0);
        if (mutation == 0) native.width = 0;
        if (mutation == 1) native.height = 0;
        if (mutation == 2) native.pixelFormat = PixelFormat::Unknown;
        if (mutation == 3) native.platformHandle.reset();
        const auto result = assembler.assemble(std::move(native));
        ASSERT_FALSE(result.hasValue());
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    }
}

TEST(AudioCaptureBlockAssemblerTest, PreservesFormatSamplesAndIndependentTimeline) {
    AudioCaptureBlockAssembler assembler{TimestampNs{ProjectClock::duration{2'000}}};
    auto native = audioBlock(96'000, {0.25F, -0.25F, 0.5F, -0.5F});
    auto samples = native.samples;

    const auto first = assembler.assemble(std::move(native));
    const auto second = assembler.assemble(audioBlock(96'480, {0.0F, 0.0F}));

    ASSERT_TRUE(first.hasValue());
    ASSERT_TRUE(second.hasValue());
    EXPECT_EQ(first.value().timestamp.time_since_epoch().count(), 2'000);
    EXPECT_EQ(second.value().timestamp.time_since_epoch().count(), 10'002'000);
    EXPECT_EQ(first.value().sampleRate, 48'000u);
    EXPECT_EQ(first.value().channels, 2u);
    EXPECT_EQ(first.value().frameCount, 2u);
    EXPECT_EQ(first.value().samples, samples);
}

TEST(AudioCaptureBlockAssemblerTest, InvalidBlockDoesNotConsumeLazyAnchor) {
    int anchorCalls = 0;
    AudioCaptureBlockAssembler assembler{[&anchorCalls] {
        ++anchorCalls;
        return TimestampNs{ProjectClock::duration{900}};
    }};
    auto invalid = audioBlock(10, {0.0F, 0.0F});
    invalid.sampleCount = 1;
    auto invalidTimestamp = audioBlock(10, {0.0F, 0.0F});
    invalidTimestamp.timestamp.timescale = 0;

    EXPECT_FALSE(assembler.assemble(std::move(invalid)).hasValue());
    EXPECT_FALSE(assembler.assemble(std::move(invalidTimestamp)).hasValue());
    EXPECT_EQ(anchorCalls, 0);
    const auto valid = assembler.assemble(audioBlock(200, {0.0F, 0.0F}));
    ASSERT_TRUE(valid.hasValue());
    EXPECT_EQ(valid.value().timestamp.time_since_epoch().count(), 900);
    EXPECT_EQ(anchorCalls, 1);
}

TEST(AudioCaptureBlockAssemblerTest, RejectsMalformedMetadataAndNonFiniteSamples) {
    AudioCaptureBlockAssembler assembler{TimestampNs{ProjectClock::duration{0}}};
    for (int mutation = 0; mutation < 6; ++mutation) {
        auto native = audioBlock(0, {0.0F, 0.0F});
        if (mutation == 0) native.sampleRate = 0;
        if (mutation == 1) native.channels = 0;
        if (mutation == 2) native.frameCount = 0;
        if (mutation == 3) native.sampleCount = 1;
        if (mutation == 4) native.samples.reset();
        if (mutation == 5) native.samples[0] =
            std::numeric_limits<float>::quiet_NaN();
        const auto result = assembler.assemble(std::move(native));
        ASSERT_FALSE(result.hasValue());
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    }
}

}  // namespace
