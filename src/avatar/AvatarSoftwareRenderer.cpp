#include "avatar/AvatarSoftwareRenderer.h"

#include "core/AppError.h"

#include <utility>

namespace creator::avatar {

core::Result<AvatarRenderFrame> AvatarSoftwareRenderer::render(
    core::TimestampNs timestamp,
    std::span<const AvatarParameterValue> parameters) {
    if (!provider_) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "avatar software renderer has no mesh provider"};
    }
    auto input = provider_(timestamp, parameters);
    if (!input.hasValue()) return input.error();
    return AvatarSoftwareRasterizer::render(
        timestamp, width_, height_, input.value().vertices, input.value().indices,
        input.value().texture);
}

}  // namespace creator::avatar
