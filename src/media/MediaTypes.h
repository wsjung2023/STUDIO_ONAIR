#pragma once

#include "core/Timebase.h"

#include <cstdint>
#include <memory>

namespace creator::media {

/// Pixel layouts the capture and encode paths exchange. Deliberately small:
/// format conversion happens only at adapter boundaries (CLAUDE.md 5), so the
/// neutral representation only needs the formats that actually cross one.
enum class PixelFormat {
    Bgra8,
    Nv12,
    P010,
    Unknown,
};

/// First release targets SDR Rec.709 end to end. HDR needs colour management
/// across capture, preview, edit and export together, so it is a separate
/// release rather than an extra enumerator (ARCHITECTURE.md 8.3).
enum class ColorSpace {
    Rec709Sdr,
};

/// Pixel-space crop inside a video frame's backing surface.
struct PixelRect final {
    std::uint32_t x{0};
    std::uint32_t y{0};
    std::uint32_t width{0};
    std::uint32_t height{0};

    friend bool operator==(const PixelRect&, const PixelRect&) = default;
};

/// A single video frame in the internal neutral representation.
///
/// Carries no FFmpeg, Qt or MLT type: this struct crosses module boundaries and
/// the layers below the application must stay free of engine types
/// (ARCHITECTURE.md 4.2, CLAUDE.md 5).
///
/// platformHandle owns the underlying GPU texture (IOSurface on macOS, D3D11
/// texture on Windows) through a shared_ptr with a type-erased deleter, so the
/// frame can be passed between the preview and encode paths without either side
/// knowing whose texture it is or when to free it. The target path keeps frames
/// on the GPU (ARCHITECTURE.md 8.1); a CPU fallback is allowed but must be
/// reported rather than hidden.
struct VideoFrame final {
    creator::core::TimestampNs timestamp{};
    std::uint32_t width{0};
    std::uint32_t height{0};
    PixelRect visibleRect{};
    std::uint32_t contentWidth{0};
    std::uint32_t contentHeight{0};
    double contentScale{1.0};
    double pointPixelScale{1.0};
    PixelFormat pixelFormat{PixelFormat::Unknown};
    ColorSpace colorSpace{ColorSpace::Rec709Sdr};
    std::shared_ptr<void> platformHandle;
};

/// A block of interleaved float audio samples.
///
/// Audio is the master stream for synchronisation (ARCHITECTURE.md 5.2), so
/// sampleRate and channels travel with the data rather than being assumed by
/// the consumer.
struct AudioBlock final {
    creator::core::TimestampNs timestamp{};
    std::uint32_t sampleRate{48000};
    std::uint32_t channels{2};
    std::uint32_t frameCount{0};
    std::shared_ptr<const float[]> samples;
};

}  // namespace creator::media
