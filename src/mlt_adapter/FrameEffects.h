#pragma once

#include "core/Result.h"
#include "domain/TimelineTypes.h"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace creator::mlt_adapter {

struct BgraFrameView final {
    std::span<const std::uint8_t> bytes;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t stride;
};

class ProcessedBgraFrame final {
public:
    [[nodiscard]] static ProcessedBgraFrame alias(
        std::span<const std::uint8_t> bytes, std::uint32_t width,
        std::uint32_t height, std::uint32_t stride);
    [[nodiscard]] static ProcessedBgraFrame own(
        std::vector<std::uint8_t> bytes, std::uint32_t width,
        std::uint32_t height, std::uint32_t stride);

    [[nodiscard]] bool aliasesInput() const noexcept { return owned_.empty(); }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint32_t stride() const noexcept { return stride_; }

private:
    ProcessedBgraFrame(std::span<const std::uint8_t> borrowed,
                       std::vector<std::uint8_t> owned,
                       std::uint32_t width, std::uint32_t height,
                       std::uint32_t stride)
        : borrowed_(borrowed), owned_(std::move(owned)), width_(width),
          height_(height), stride_(stride) {}

    std::span<const std::uint8_t> borrowed_;
    std::vector<std::uint8_t> owned_;
    std::uint32_t width_;
    std::uint32_t height_;
    std::uint32_t stride_;
};

[[nodiscard]] core::Result<ProcessedBgraFrame> applyVisualTransform(
    BgraFrameView source, std::uint32_t canvasWidth,
    std::uint32_t canvasHeight, const domain::VisualTransform& transform);

[[nodiscard]] core::Result<void> applyAudioEnvelope(
    std::span<float> interleavedSamples, std::uint32_t channels,
    std::uint64_t firstClipFrame, std::uint64_t totalClipFrames,
    std::uint32_t sampleRate, const domain::AudioEnvelope& envelope);

}  // namespace creator::mlt_adapter
