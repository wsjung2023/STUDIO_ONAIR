#include "avatar/inochi2d/Inochi2dAvatarRenderer.h"

#include "avatar/AvatarSoftwareRasterizer.h"
#include "core/AppError.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>
#include <vector>

namespace creator::avatar::inochi2d {
namespace {

// The runtime emits vertices in the puppet's own pixel space. Inochi2D models are
// authored full-height with the head at the top (smallest Y) and legs at the
// bottom, but a VTuber overlay wants head-and-shoulders -- fitting the whole body
// shrinks the character to an unrecognisable sliver in the frame. So crop to the
// upper band of the puppet (head + chest), tighten horizontally to what actually
// falls in that band, and fit that region centred in the frame with a small
// margin, preserving aspect ratio. Vertices below the crop are transformed with
// the same scale/offset and simply fall outside the frame (the rasteriser clips
// them), so no geometry is mutated -- only the framing changes.
void fitBatchesToFrame(std::vector<AvatarSoftwareRenderInput>& batches,
                       std::uint32_t width, std::uint32_t height,
                       float userScale, float posX, float posY) {
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
    const float fullHeight = maxY - minY;
    if (!(maxX - minX > 0.0F) || !(fullHeight > 0.0F)) return;

    // Keep the top portion (head + shoulders/upper chest). Tuned to Inochi2D
    // full-body puppets; ~0.55 lands a natural head-and-upper-body VTuber shot
    // (0.3 crops into the face, 1.0 shows the whole body). Overridable via
    // CS_INOCHI2D_CROP for tuning without a rebuild.
    float upperBodyFraction = 0.55F;
    {
        char buf[16] = {0};
        std::size_t len = 0;
#ifdef _WIN32
        if (getenv_s(&len, buf, sizeof buf, "CS_INOCHI2D_CROP") == 0 && len > 0) {
#else
        const char* env = std::getenv("CS_INOCHI2D_CROP");
        if (env != nullptr && (std::snprintf(buf, sizeof buf, "%s", env), true)) {
#endif
            const float parsed = std::strtof(buf, nullptr);
            if (parsed > 0.05F && parsed <= 1.0F) upperBodyFraction = parsed;
        }
    }
    const float cropBottom = minY + upperBodyFraction * fullHeight;

    // Horizontal extent of just the upper band -- the head/shoulders are much
    // narrower than the full arm-span or leg stance, so this keeps the head large.
    float cropMinX = std::numeric_limits<float>::max();
    float cropMaxX = std::numeric_limits<float>::lowest();
    for (const auto& batch : batches) {
        for (const auto& vertex : batch.vertices) {
            if (vertex.y <= cropBottom) {
                cropMinX = std::min(cropMinX, vertex.x);
                cropMaxX = std::max(cropMaxX, vertex.x);
            }
        }
    }
    if (!(cropMaxX > cropMinX)) {  // degenerate band -- fall back to full width
        cropMinX = minX;
        cropMaxX = maxX;
    }

    const float boundsWidth = cropMaxX - cropMinX;
    const float boundsHeight = cropBottom - minY;
    if (!(boundsWidth > 0.0F) || !(boundsHeight > 0.0F)) return;

    constexpr float kMargin = 0.06F;  // 6% breathing room on each edge
    const float availableWidth = static_cast<float>(width) * (1.0F - 2.0F * kMargin);
    const float availableHeight = static_cast<float>(height) * (1.0F - 2.0F * kMargin);
    // Base fit, then the editor's live size multiplier and normalised position.
    const float scale =
        std::min(availableWidth / boundsWidth, availableHeight / boundsHeight) *
        userScale;
    const float centreX = (cropMinX + cropMaxX) * 0.5F;
    const float centreY = (minY + cropBottom) * 0.5F;
    const float frameCentreX = static_cast<float>(width) * posX;
    const float frameCentreY = static_cast<float>(height) * posY;
    for (auto& batch : batches) {
        for (auto& vertex : batch.vertices) {
            vertex.x = (vertex.x - centreX) * scale + frameCentreX;
            vertex.y = (vertex.y - centreY) * scale + frameCentreY;
        }
    }
}

// Gentle idle motion so a puppet with no live tracking still feels alive: a slow
// vertical "breath" plus a slighter, slower horizontal sway shift the whole
// framed puppet by a few percent. Subtle enough to sit under real facial
// tracking once a rigged model drives the expression parameters.
void applyIdleMotion(std::vector<AvatarSoftwareRenderInput>& batches,
                     std::uint32_t width, std::uint32_t height,
                     core::TimestampNs timestamp) {
    const double seconds =
        static_cast<double>(timestamp.time_since_epoch().count()) / 1'000'000'000.0;
    const float breatheY = static_cast<float>(std::sin(seconds * 1.9)) *
                           static_cast<float>(height) * 0.02F;
    const float swayX = static_cast<float>(std::sin(seconds * 0.8)) *
                        static_cast<float>(width) * 0.01F;
    for (auto& batch : batches) {
        for (auto& vertex : batch.vertices) {
            vertex.x += swayX;
            vertex.y += breatheY;
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
    fitBatchesToFrame(batches.value(), width_, height_, userScale(), posX(),
                      posY());
    applyIdleMotion(batches.value(), width_, height_, timestamp);
    auto frame = AvatarSoftwareRasterizer::renderBatches(
        timestamp, width_, height_, batches.value());
    if (!frame.hasValue()) return frame.error();
    lastTimestamp_ = timestamp;
    return frame;
}

std::vector<AvatarParameterBinding> inochi2dAvatarBindings() {
    using Source = AvatarParameterSource;
    // Standard Inochi2D parameter names (used by the CC0 Arch-chan model). Eye
    // "Open" and jaw/mouth are 0..1; head tilt is -1..1. Head position is a 2D
    // parameter the runtime drives via its own physics, so it is not bound here.
    return {
        {"Eye:: Left:: Open", Source::EyeOpenLeft, 1.0F, 0.0F, 0.0F, 1.0F},
        {"Eye:: Right:: Open", Source::EyeOpenRight, 1.0F, 0.0F, 0.0F, 1.0F},
        {"Eyebrow:: Left", Source::BrowUpLeft, 1.0F, 0.0F, 0.0F, 1.0F},
        {"Eyebrow:: Right", Source::BrowUpRight, 1.0F, 0.0F, 0.0F, 1.0F},
        {"Mouth:: Jaw Open", Source::MouthOpen, 1.0F, 0.0F, 0.0F, 1.0F},
        {"Mouth:: Pucker / Widen", Source::MouthWide, 1.0F, 0.0F, 0.0F, 1.0F},
        {"Head Tilt", Source::HeadRoll, 1.0F, 0.0F, -1.0F, 1.0F},
    };
}

}  // namespace creator::avatar::inochi2d
