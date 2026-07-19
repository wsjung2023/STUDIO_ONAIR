#include "ffmpeg_adapter/BgraFrameMappers.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <limits>

namespace {

using creator::ffmpeg_adapter::CpuBgraFrameBuffer;
using creator::ffmpeg_adapter::CpuBgraFrameMapper;

TEST(CpuBgraFrameMapperTest, MapsOwnedStrideAndKeepsStorageAlive) {
    auto buffer = CpuBgraFrameBuffer::create(4, 3, 24).value();
    std::fill_n(buffer->data(), buffer->size(), std::uint8_t{42});
    creator::media::VideoFrame frame{
        .timestamp = creator::core::TimestampNs{std::chrono::milliseconds{7}},
        .width = 4,
        .height = 3,
        .pixelFormat = creator::media::PixelFormat::Bgra8,
        .platformHandle = buffer,
    };
    CpuBgraFrameMapper mapper;

    auto mapped = mapper.map(frame);
    frame.platformHandle.reset();
    buffer.reset();

    ASSERT_TRUE(mapped.hasValue()) << mapped.error().message();
    EXPECT_EQ(mapped.value().rowBytes, 24u);
    EXPECT_EQ(mapped.value().data[0], 42u);
    EXPECT_TRUE(mapped.value().owner);
}

TEST(CpuBgraFrameMapperTest, RejectsMismatchedFrameMetadata) {
    auto buffer = CpuBgraFrameBuffer::create(4, 3).value();
    creator::media::VideoFrame frame{
        .width = 8,
        .height = 3,
        .pixelFormat = creator::media::PixelFormat::Bgra8,
        .platformHandle = std::move(buffer),
    };
    CpuBgraFrameMapper mapper;

    const auto result = mapper.map(frame);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), creator::core::ErrorCode::InvalidArgument);
}

TEST(CpuBgraFrameBufferTest, RejectsStrideAndSizeOverflow) {
    EXPECT_FALSE(CpuBgraFrameBuffer::create(4, 3, 15).hasValue());
    EXPECT_FALSE(CpuBgraFrameBuffer::create(
                     4, 3, std::numeric_limits<std::size_t>::max())
                     .hasValue());
}

}  // namespace
