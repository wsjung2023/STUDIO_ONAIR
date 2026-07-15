#pragma once

#include "core/Timebase.h"

#include <cstdint>
#include <memory>

namespace creator::media {

enum class PixelFormat {
    Bgra8,
    Nv12,
    P010,
    Unknown,
};

enum class ColorSpace {
    Rec709Sdr,
};

struct VideoFrame final {
    creator::core::TimestampNs timestamp{};
    std::uint32_t width{0};
    std::uint32_t height{0};
    PixelFormat pixelFormat{PixelFormat::Unknown};
    ColorSpace colorSpace{ColorSpace::Rec709Sdr};
    std::shared_ptr<void> platformHandle;
};

struct AudioBlock final {
    creator::core::TimestampNs timestamp{};
    std::uint32_t sampleRate{48000};
    std::uint32_t channels{2};
    std::uint32_t frameCount{0};
    std::shared_ptr<const float[]> samples;
};

}  // namespace creator::media
