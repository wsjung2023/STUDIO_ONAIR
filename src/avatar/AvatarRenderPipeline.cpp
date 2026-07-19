#include "avatar/AvatarRenderPipeline.h"

#include "core/AppError.h"

namespace creator::avatar {

core::Result<AvatarRenderFrame> AvatarRenderPipeline::render(
    const AvatarMotionSample& sample) const {
    if (mapper_ == nullptr || renderer_ == nullptr) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "avatar render pipeline is unavailable"};
    }
    const auto parameters = mapper_->map(sample.parameters);
    if (!parameters.hasValue()) return parameters.error();
    return renderer_->render(sample.timestamp, parameters.value());
}

}  // namespace creator::avatar
