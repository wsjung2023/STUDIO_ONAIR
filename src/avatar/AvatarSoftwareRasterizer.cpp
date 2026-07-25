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

std::uint8_t toByte(float value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(value * 255.0F, 0.0F, 255.0F));
}

// Separable blend function B(cb, cs) for the blend modes a rigged puppet
// actually uses. Unhandled modes fall through to Normal (return the source),
// which composites as plain source-over.
float separableBlend(std::uint32_t mode, float cb, float cs) noexcept {
    switch (mode) {
        case 0x01: return cb * cs;                              // multiply
        case 0x02: return cb + cs - cb * cs;                    // screen
        case 0x04: return std::min(cb, cs);                     // darken
        case 0x05: return std::max(cb, cs);                     // lighten
        case 0x07:                                              // linear dodge
        case 0x08: return std::min(1.0F, cb + cs);              // add / glow
        case 0x0E: return std::max(0.0F, cb + cs - 1.0F);       // subtract
        default: return cs;                                     // normal
    }
}

// Composites one straight-alpha source sample onto a straight-alpha BGRA
// destination pixel in place, honouring the blend mode. Porter-Duff IN/OUT
// operators (used to clip parts to the layer below) are handled explicitly; all
// other modes use the W3C source-over-with-blend formula.
void compositePixel(std::uint8_t* dst, float srcB, float srcG, float srcR,
                    float srcA, std::uint32_t mode) noexcept {
    const float dstB = static_cast<float>(dst[0]) / 255.0F;
    const float dstG = static_cast<float>(dst[1]) / 255.0F;
    const float dstR = static_cast<float>(dst[2]) / 255.0F;
    const float dstA = static_cast<float>(dst[3]) / 255.0F;

    if (mode == 0x11) {  // source-in: keep the source only where dst is present
        dst[0] = toByte(srcB);
        dst[1] = toByte(srcG);
        dst[2] = toByte(srcR);
        dst[3] = toByte(srcA * dstA);
        return;
    }
    if (mode == 0x10) {  // destination-in: keep dst only where src is present
        dst[3] = toByte(dstA * srcA);
        return;
    }
    if (mode == 0x12) {  // source-out: keep the source only where dst is absent
        dst[0] = toByte(srcB);
        dst[1] = toByte(srcG);
        dst[2] = toByte(srcR);
        dst[3] = toByte(srcA * (1.0F - dstA));
        return;
    }

    const float outA = srcA + dstA * (1.0F - srcA);
    if (outA <= 0.0F) {
        dst[0] = dst[1] = dst[2] = dst[3] = 0;
        return;
    }
    const auto channel = [&](float cs, float cb) {
        const float blended = separableBlend(mode, cb, cs);
        const float co = srcA * (1.0F - dstA) * cs + srcA * dstA * blended +
                         (1.0F - srcA) * dstA * cb;
        return toByte(co / outA);
    };
    dst[0] = channel(srcB, dstB);
    dst[1] = channel(srcG, dstG);
    dst[2] = channel(srcR, dstR);
    dst[3] = toByte(outA);
}

struct TextureSample final {
    float b{0.0F};
    float g{0.0F};
    float r{0.0F};
    float a{0.0F};
};

TextureSample sampleTexture(const AvatarTexture& texture, float u, float v) noexcept {
    const auto tx = static_cast<std::uint32_t>(std::min<float>(
        static_cast<float>(texture.width - 1U),
        std::floor(std::clamp(u, 0.0F, 1.0F) *
                       static_cast<float>(texture.width - 1U) + 0.5F)));
    const auto ty = static_cast<std::uint32_t>(std::min<float>(
        static_cast<float>(texture.height - 1U),
        std::floor(std::clamp(v, 0.0F, 1.0F) *
                       static_cast<float>(texture.height - 1U) + 0.5F)));
    const auto offset =
        (static_cast<std::size_t>(ty) * texture.width + tx) * 4U;
    return {static_cast<float>(texture.bgra[offset]) / 255.0F,
            static_cast<float>(texture.bgra[offset + 1U]) / 255.0F,
            static_cast<float>(texture.bgra[offset + 2U]) / 255.0F,
            static_cast<float>(texture.bgra[offset + 3U]) / 255.0F};
}

// Walks a command's triangles once, invoking `plot(x, y, sample)` for every
// covered pixel whose interpolated UV samples the source texture. Shared by the
// colour and mask passes.
template <typename Plot>
void rasterizeTriangles(std::uint32_t width, std::uint32_t height,
                        const AvatarSoftwareRenderInput& command, Plot&& plot) {
    const auto& texture = command.texture;
    if (texture.width == 0U || texture.height == 0U) return;
    if (static_cast<std::uint64_t>(texture.width) * texture.height * 4U !=
        texture.bgra.size()) {
        return;
    }
    const auto& vertices = command.vertices;
    const auto& indices = command.indices;
    for (std::size_t triangle = 0; triangle + 2U < indices.size();
         triangle += 3U) {
        if (indices[triangle] >= vertices.size() ||
            indices[triangle + 1U] >= vertices.size() ||
            indices[triangle + 2U] >= vertices.size()) {
            continue;
        }
        const auto& first = vertices[indices[triangle]];
        const auto& second = vertices[indices[triangle + 1U]];
        const auto& third = vertices[indices[triangle + 2U]];
        const float area = edge(first, second, third.x, third.y);
        if (std::abs(area) < 1.0e-6F) continue;

        const auto minX = std::max(0, static_cast<int>(std::floor(
                                          std::min({first.x, second.x, third.x}))));
        const auto maxX = std::min(static_cast<int>(width) - 1,
                                   static_cast<int>(std::ceil(std::max(
                                       {first.x, second.x, third.x}))));
        const auto minY = std::max(0, static_cast<int>(std::floor(
                                          std::min({first.y, second.y, third.y}))));
        const auto maxY = std::min(static_cast<int>(height) - 1,
                                   static_cast<int>(std::ceil(std::max(
                                       {first.y, second.y, third.y}))));
        if (minX > maxX || minY > maxY) continue;

        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                const float sampleX = static_cast<float>(x) + 0.5F;
                const float sampleY = static_cast<float>(y) + 0.5F;
                const float w0 = edge(second, third, sampleX, sampleY) / area;
                const float w1 = edge(third, first, sampleX, sampleY) / area;
                const float w2 = edge(first, second, sampleX, sampleY) / area;
                if (w0 < 0.0F || w1 < 0.0F || w2 < 0.0F) continue;
                const float u = w0 * first.u + w1 * second.u + w2 * third.u;
                const float v = w0 * first.v + w1 * second.v + w2 * third.v;
                plot(x, y, sampleTexture(texture, u, v));
            }
        }
    }
}

// Draws a command's triangles into a straight-alpha BGRA buffer, optionally
// clipped by a single-channel mask (mask mode 0 keeps the covered area, 1 dodges
// it), using the command's blend mode.
void drawCommand(std::vector<std::uint8_t>& buffer, std::uint32_t width,
                 std::uint32_t height, const AvatarSoftwareRenderInput& command,
                 const std::vector<std::uint8_t>* mask,
                 std::uint32_t maskMode) {
    rasterizeTriangles(
        width, height, command,
        [&](int x, int y, const TextureSample& s) {
            float alpha = s.a;
            if (mask != nullptr) {
                const float coverage =
                    static_cast<float>((*mask)[static_cast<std::size_t>(y) * width +
                                               static_cast<std::size_t>(x)]) /
                    255.0F;
                alpha *= (maskMode == 1U) ? (1.0F - coverage) : coverage;
            }
            if (alpha <= 0.0F) return;
            const auto offset =
                (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) *
                4U;
            compositePixel(&buffer[offset], s.b, s.g, s.r, alpha,
                           command.blendMode);
        });
}

// Accumulates a mask command's coverage (maximum source alpha) into a
// single-channel buffer.
void drawMask(std::vector<std::uint8_t>& mask, std::uint32_t width,
              std::uint32_t height, const AvatarSoftwareRenderInput& command) {
    rasterizeTriangles(width, height, command,
                       [&](int x, int y, const TextureSample& s) {
                           auto& cell =
                               mask[static_cast<std::size_t>(y) * width +
                                    static_cast<std::size_t>(x)];
                           cell = std::max(cell, toByte(s.a));
                       });
}

// Composites a whole offscreen composite buffer onto a destination buffer with
// the blit's blend mode (used for composite-blit).
void blitBuffer(std::vector<std::uint8_t>& dst,
                const std::vector<std::uint8_t>& src, std::uint32_t blendMode) {
    for (std::size_t offset = 0; offset + 3U < src.size(); offset += 4U) {
        const float srcA = static_cast<float>(src[offset + 3U]) / 255.0F;
        if (srcA <= 0.0F) continue;
        compositePixel(&dst[offset], static_cast<float>(src[offset]) / 255.0F,
                       static_cast<float>(src[offset + 1U]) / 255.0F,
                       static_cast<float>(src[offset + 2U]) / 255.0F, srcA,
                       blendMode);
    }
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
    if (textureWidth * textureHeight * 4U != texture.bgra.size()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "avatar texture storage is not tightly packed BGRA"};
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

    AvatarSoftwareRenderInput command;
    command.vertices.assign(vertices.begin(), vertices.end());
    command.indices.assign(indices.begin(), indices.end());
    command.texture = texture;
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(width) * height * 4U, 0U);
    drawCommand(pixels, width, height, command, nullptr, 0U);
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
    const auto frameBytes = static_cast<std::size_t>(frameWidth * frameHeight * 4U);

    // Stateful compositor matching the Inochi2D v0.8.7 draw-list protocol.
    // `targets` is a stack of straight-alpha BGRA buffers; the base is the frame
    // and each composite group pushes an offscreen buffer. `mask` is the shared
    // single-channel coverage for a define-mask/masked-draw run.
    std::vector<std::vector<std::uint8_t>> targets;
    targets.emplace_back(frameBytes, 0U);
    std::vector<std::vector<std::uint8_t>> pendingComposites;
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(frameWidth * frameHeight),
                                   0U);
    bool inMaskRun = false;

    for (const auto& command : batches) {
        switch (static_cast<AvatarDrawState>(command.state)) {
            case AvatarDrawState::CompositeBegin:
                inMaskRun = false;
                targets.emplace_back(frameBytes, 0U);
                break;
            case AvatarDrawState::CompositeEnd:
                inMaskRun = false;
                if (targets.size() > 1U) {
                    pendingComposites.push_back(std::move(targets.back()));
                    targets.pop_back();
                }
                break;
            case AvatarDrawState::CompositeBlit:
                inMaskRun = false;
                if (!pendingComposites.empty()) {
                    blitBuffer(targets.back(), pendingComposites.back(),
                               command.blendMode);
                    pendingComposites.pop_back();
                }
                break;
            case AvatarDrawState::DefineMask:
                if (!inMaskRun) {
                    std::fill(mask.begin(), mask.end(), std::uint8_t{0});
                    inMaskRun = true;
                }
                drawMask(mask, width, height, command);
                break;
            case AvatarDrawState::MaskedDraw:
                inMaskRun = false;
                drawCommand(targets.back(), width, height, command, &mask,
                            command.maskMode);
                break;
            case AvatarDrawState::Normal:
            default:
                inMaskRun = false;
                drawCommand(targets.back(), width, height, command, nullptr, 0U);
                break;
        }
    }

    // A well-formed draw list balances every composite; if it did not, the
    // outstanding offscreen buffers are the best available result.
    while (targets.size() > 1U) {
        blitBuffer(targets.front(), targets.back(), 0U);
        targets.pop_back();
    }
    return AvatarRenderFrame::fromBgra(timestamp, width, height, width * 4U,
                                       std::move(targets.front()));
}

}  // namespace creator::avatar
