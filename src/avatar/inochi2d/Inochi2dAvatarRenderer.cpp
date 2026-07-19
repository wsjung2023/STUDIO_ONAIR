#include "avatar/inochi2d/Inochi2dAvatarRenderer.h"

#include "core/AppError.h"

#include <cmath>
#include <utility>

namespace creator::avatar::inochi2d {

core::Result<std::unique_ptr<Inochi2dAvatarRenderer>>
Inochi2dAvatarRenderer::open(const std::filesystem::path& libraryPath,
                             const std::filesystem::path& modelPath,
                             std::uint32_t width, std::uint32_t height) {
    auto runtime = Inochi2dModelRuntime::open(libraryPath, modelPath);
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
    auto frame = AvatarSoftwareRasterizer::renderBatches(
        timestamp, width_, height_, batches.value());
    if (!frame.hasValue()) return frame.error();
    lastTimestamp_ = timestamp;
    return frame;
}

}  // namespace creator::avatar::inochi2d
