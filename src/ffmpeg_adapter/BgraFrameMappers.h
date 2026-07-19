#pragma once

#include "recorder/TrackSegmentPorts.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace creator::ffmpeg_adapter {

class CpuBgraFrameBuffer final {
public:
    [[nodiscard]] static core::Result<std::shared_ptr<CpuBgraFrameBuffer>> create(
        std::uint32_t width, std::uint32_t height, std::size_t rowBytes = 0);

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::size_t rowBytes() const noexcept { return rowBytes_; }
    [[nodiscard]] std::uint8_t* data() noexcept { return bytes_.data(); }
    [[nodiscard]] const std::uint8_t* data() const noexcept { return bytes_.data(); }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

private:
    CpuBgraFrameBuffer(std::uint32_t width, std::uint32_t height,
                       std::size_t rowBytes, std::vector<std::uint8_t> bytes);

    std::uint32_t width_{0};
    std::uint32_t height_{0};
    std::size_t rowBytes_{0};
    std::vector<std::uint8_t> bytes_;
};

class CpuBgraFrameMapper final : public recorder::IVideoFrameMapper {
public:
    [[nodiscard]] core::Result<recorder::MappedVideoFrame> map(
        const media::VideoFrame& frame) override;
};

class MacCvPixelBufferFrameMapper final : public recorder::IVideoFrameMapper {
public:
    [[nodiscard]] core::Result<recorder::MappedVideoFrame> map(
        const media::VideoFrame& frame) override;
};

}  // namespace creator::ffmpeg_adapter
