#include "avatar/inochi2d/Inochi2dAvatarRenderer.h"

#include "avatar/AvatarSoftwareRasterizer.h"
#include "core/AppError.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace creator::avatar::inochi2d {
namespace {

// The runtime emits vertices in the puppet's own pixel space, which is usually
// larger than -- and off-centre from -- the target frame (a raw pass would clip
// the head and show only a slice of the body). Normalise every vertex so the
// puppet's bounding box sits centred within the frame with a small margin,
// preserving aspect ratio, so the whole character is visible and consistently
// framed regardless of how the source puppet was authored.
void fitBatchesToFrame(std::vector<AvatarSoftwareRenderInput>& batches,
                       std::uint32_t width, std::uint32_t height) {
    if (width == 0U || height == 0U) return;
    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = std::numeric_limits<float>::lowest();
    for (const auto& batch : batches) {
        for (const auto& vertex : batch.vertices) {
            minX = std::min(minX, vertex.x);
            maxX = std::max(maxX, vertex.x);
            minY = std::min(minY, vertex.y);
            maxY = std::max(maxY, vertex.y);
        }
    }
    const float boundsWidth = maxX - minX;
    const float boundsHeight = maxY - minY;
    if (!(boundsWidth > 0.0F) || !(boundsHeight > 0.0F)) return;

    constexpr float kMargin = 0.06F;  // 6% breathing room on each edge
    const float availableWidth = static_cast<float>(width) * (1.0F - 2.0F * kMargin);
    const float availableHeight = static_cast<float>(height) * (1.0F - 2.0F * kMargin);
    const float scale =
        std::min(availableWidth / boundsWidth, availableHeight / boundsHeight);
    const float centreX = (minX + maxX) * 0.5F;
    const float centreY = (minY + maxY) * 0.5F;
    const float frameCentreX = static_cast<float>(width) * 0.5F;
    const float frameCentreY = static_cast<float>(height) * 0.5F;
    for (auto& batch : batches) {
        for (auto& vertex : batch.vertices) {
            vertex.x = (vertex.x - centreX) * scale + frameCentreX;
            vertex.y = (vertex.y - centreY) * scale + frameCentreY;
        }
    }
}

}  // namespace

core::Result<std::unique_ptr<Inochi2dAvatarRenderer>>
Inochi2dAvatarRenderer::open(const std::filesystem::path& runtimeRoot,
                             const std::filesystem::path& modelPath,
                             std::uint32_t width, std::uint32_t height) {
    auto runtime = Inochi2dModelRuntime::open(runtimeRoot, modelPath);
    if (!runtime.hasValue()) return runtime.error();
    return std::unique_ptr<Inochi2dAvatarRenderer>{
        new Inochi2dAvatarRenderer{std::move(runtime).value(), width, height}};
}

core::Result<AvatarRenderFrame> Inochi2dAvatarRenderer::render(
    core::TimestampNs timestamp,
    std::span<const AvatarParameterValue> parameters) {
    if (!runtime_) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "Inochi2D avatar renderer is not loaded"};
    }
    if (lastTimestamp_.has_value() && timestamp < *lastTimestamp_) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Inochi2D render timestamp moved backwards"};
    }
    float deltaSeconds = 0.0F;
    if (lastTimestamp_.has_value()) {
        const auto delta = timestamp - *lastTimestamp_;
        deltaSeconds = static_cast<float>(delta.count()) / 1'000'000'000.0F;
        if (!std::isfinite(deltaSeconds)) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "Inochi2D render timestamp delta is invalid"};
        }
    }
    auto applied = runtime_->applyParameters(parameters);
    if (!applied.hasValue()) return applied.error();
    auto batches = runtime_->renderSnapshot(deltaSeconds);
    if (!batches.hasValue()) return batches.error();
    fitBatchesToFrame(batches.value(), width_, height_);
    auto frame = AvatarSoftwareRasterizer::renderBatches(
        timestamp, width_, height_, batches.value());
    if (!frame.hasValue()) return frame.error();
    lastTimestamp_ = timestamp;
    return frame;
}

}  // namespace creator::avatar::inochi2d
