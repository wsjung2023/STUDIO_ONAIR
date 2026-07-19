#pragma once

#include "core/Result.h"
#include "core/Timebase.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace creator::avatar {

/// CPU BGRA8 frame emitted by an avatar renderer.
///
/// Zeroed pixels are transparent black, so a renderer can safely return a
/// transparent frame while a model is loading or when no face is present.
/// Ownership is shared and immutable after construction, which lets preview
/// and recording consumers retain the frame without a renderer lock.
class AvatarRenderFrame final {
public:
    [[nodiscard]] static core::Result<AvatarRenderFrame> transparent(
        core::TimestampNs timestamp, std::uint32_t width,
        std::uint32_t height);
    [[nodiscard]] static core::Result<AvatarRenderFrame> fromBgra(
        core::TimestampNs timestamp, std::uint32_t width,
        std::uint32_t height, std::uint32_t stride,
        std::vector<std::uint8_t> bytes);

    [[nodiscard]] core::TimestampNs timestamp() const noexcept { return timestamp_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint32_t stride() const noexcept { return stride_; }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;

    friend bool operator==(const AvatarRenderFrame&, const AvatarRenderFrame&) = default;

private:
    AvatarRenderFrame(core::TimestampNs timestamp, std::uint32_t width,
                      std::uint32_t height, std::uint32_t stride,
                      std::shared_ptr<const std::vector<std::uint8_t>> bytes)
        : timestamp_(timestamp), width_(width), height_(height), stride_(stride),
          bytes_(std::move(bytes)) {}

    core::TimestampNs timestamp_;
    std::uint32_t width_;
    std::uint32_t height_;
    std::uint32_t stride_;
    std::shared_ptr<const std::vector<std::uint8_t>> bytes_;
};

}  // namespace creator::avatar
