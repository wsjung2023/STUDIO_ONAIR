#include "avatar/AvatarSoftwareRasterizer.h"

#include "core/AppError.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace creator::avatar {
namespace {

float edge(const AvatarMeshVertex& first, const AvatarMeshVertex& second,
           float x, float y) noexcept {
    return (x - first.x) * (second.y - first.y) -
           (y - first.y) * (second.x - first.x);
}

std::uint8_t blendChannel(std::uint8_t source, std::uint8_t destination,
                          float sourceAlpha, float destinationAlpha,
                          float outputAlpha) noexcept {
    if (outputAlpha <= 0.0F) return 0;
    const auto value = (static_cast<float>(source) * sourceAlpha +
                        static_cast<float>(destination) * destinationAlpha *
                            (1.0F - sourceAlpha)) /
                       outputAlpha;
    return static_cast<std::uint8_t>(std::clamp(value, 0.0F, 255.0F));
}

}  // namespace

core::Result<AvatarRenderFrame> AvatarSoftwareRasterizer::render(
    core::TimestampNs timestamp, std::uint32_t width, std::uint32_t height,
    std::span<const AvatarMeshVertex> vertices,
    std::span<const std::uint32_t> indices, const AvatarTexture& texture) {
    if (width == 0U || height == 0U || vertices.empty() || indices.empty() ||
        indices.size() % 3U != 0U || texture.width == 0U || texture.height == 0U) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "avatar rasterizer received an empty mesh"};
    }
    const auto textureWidth = static_cast<std::uint64_t>(texture.width);
    const auto textureHeight = static_cast<std::uint64_t>(texture.height);
    if (textureWidth > std::numeric_limits<std::uint64_t>::max() /
                          (textureHeight * 4U)) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "avatar texture dimensions overflow storage"};
    }
    const auto expectedTextureBytes = textureWidth * textureHeight * 4U;
    if (expectedTextureBytes != texture.bgra.size()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "avatar texture storage is not tightly packed BGRA"};
    }
    const auto frameWidth = static_cast<std::uint64_t>(width);
    const auto frameHeight = static_cast<std::uint64_t>(height);
    if (frameWidth > std::numeric_limits<std::uint64_t>::max() /
                        (frameHeight * 4U)) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "avatar rasterizer frame dimensions overflow storage"};
    }
    const auto expectedFrameBytes = frameWidth * frameHeight * 4U;
    if (expectedFrameBytes > std::numeric_limits<std::size_t>::max()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "avatar rasterizer frame is too large"};
    }
    for (const auto& vertex : vertices) {
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) ||
            !std::isfinite(vertex.u) || !std::isfinite(vertex.v)) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "avatar mesh contains a non-finite vertex"};
        }
    }
    for (const auto index : indices) {
        if (index >= vertices.size()) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "avatar mesh index is outside the vertex list"};
        }
    }

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(expectedFrameBytes), 0U);
    for (std::size_t triangle = 0; triangle < indices.size(); triangle += 3U) {
        const auto& first = vertices[indices[triangle]];
        const auto& second = vertices[indices[triangle + 1U]];
        const auto& third = vertices[indices[triangle + 2U]];
        const float area = edge(first, second, third.x, third.y);
        if (std::abs(area) < 1.0e-6F) continue;

        const auto minX = std::max(0, static_cast<int>(std::floor(
                                         std::min({first.x, second.x, third.x}))));
        const auto maxX = std::min(static_cast<int>(width) - 1,
                                   static_cast<int>(std::ceil(
                                       std::max({first.x, second.x, third.x}))));
        const auto minY = std::max(0, static_cast<int>(std::floor(
                                         std::min({first.y, second.y, third.y}))));
        const auto maxY = std::min(static_cast<int>(height) - 1,
                                   static_cast<int>(std::ceil(
                                       std::max({first.y, second.y, third.y}))));
        if (minX > maxX || minY > maxY) continue;

        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                const float sampleX = static_cast<float>(x) + 0.5F;
                const float sampleY = static_cast<float>(y) + 0.5F;
                const float firstWeight = edge(second, third, sampleX, sampleY) / area;
                const float secondWeight = edge(third, first, sampleX, sampleY) / area;
                const float thirdWeight = edge(first, second, sampleX, sampleY) / area;
                if (firstWeight < 0.0F || secondWeight < 0.0F || thirdWeight < 0.0F)
                    continue;

                const float u = std::clamp(firstWeight * first.u +
                                               secondWeight * second.u +
                                               thirdWeight * third.u,
                                           0.0F, 1.0F);
                const float v = std::clamp(firstWeight * first.v +
                                               secondWeight * second.v +
                                               thirdWeight * third.v,
                                           0.0F, 1.0F);
                const auto textureX = static_cast<std::uint32_t>(
                    std::min<float>(static_cast<float>(texture.width - 1U),
                                    std::floor(u * static_cast<float>(texture.width - 1U) +
                                               0.5F)));
                const auto textureY = static_cast<std::uint32_t>(
                    std::min<float>(static_cast<float>(texture.height - 1U),
                                    std::floor(v * static_cast<float>(texture.height - 1U) +
                                               0.5F)));
                const auto sourceOffset =
                    (static_cast<std::size_t>(textureY) * texture.width + textureX) * 4U;
                const auto destinationOffset =
                    (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4U;
                const float sourceAlpha = texture.bgra[sourceOffset + 3U] / 255.0F;
                const float destinationAlpha = pixels[destinationOffset + 3U] / 255.0F;
                const float outputAlpha = sourceAlpha +
                                          destinationAlpha * (1.0F - sourceAlpha);
                for (std::size_t channel = 0; channel < 3U; ++channel) {
                    pixels[destinationOffset + channel] = blendChannel(
                        texture.bgra[sourceOffset + channel],
                        pixels[destinationOffset + channel], sourceAlpha,
                        destinationAlpha, outputAlpha);
                }
                pixels[destinationOffset + 3U] = static_cast<std::uint8_t>(
                    std::clamp(outputAlpha * 255.0F, 0.0F, 255.0F));
            }
        }
    }
    return AvatarRenderFrame::fromBgra(timestamp, width, height, width * 4U,
                                       std::move(pixels));
}

core::Result<AvatarRenderFrame> AvatarSoftwareRasterizer::renderBatches(
    core::TimestampNs timestamp, std::uint32_t width, std::uint32_t height,
    std::span<const AvatarSoftwareRenderInput> batches) {
    if (batches.empty()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "avatar rasterizer received no draw batches"};
    }
    const auto frameWidth = static_cast<std::uint64_t>(width);
    const auto frameHeight = static_cast<std::uint64_t>(height);
    if (width == 0U || height == 0U ||
        frameWidth > std::numeric_limits<std::uint64_t>::max() /
                         (frameHeight * 4U)) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "avatar rasterizer frame dimensions overflow storage"};
    }
    const auto frameBytes = frameWidth * frameHeight * 4U;
    if (frameBytes > std::numeric_limits<std::size_t>::max()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "avatar rasterizer frame is too large"};
    }
    std::vector<std::uint8_t> composited(static_cast<std::size_t>(frameBytes), 0U);
    for (const auto& batch : batches) {
        auto rendered = render(timestamp, width, height, batch.vertices,
                               batch.indices, batch.texture);
        if (!rendered.hasValue()) return rendered.error();
        const auto bytes = rendered.value().bytes();
        for (std::size_t offset = 0; offset < composited.size(); offset += 4U) {
            const float sourceAlpha = bytes[offset + 3U] / 255.0F;
            const float destinationAlpha = composited[offset + 3U] / 255.0F;
            const float outputAlpha = sourceAlpha +
                                      destinationAlpha * (1.0F - sourceAlpha);
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                composited[offset + channel] = blendChannel(
                    bytes[offset + channel], composited[offset + channel],
                    sourceAlpha, destinationAlpha, outputAlpha);
            }
            composited[offset + 3U] = static_cast<std::uint8_t>(
                std::clamp(outputAlpha * 255.0F, 0.0F, 255.0F));
        }
    }
    return AvatarRenderFrame::fromBgra(timestamp, width, height, width * 4U,
                                       std::move(composited));
}

}  // namespace creator::avatar
