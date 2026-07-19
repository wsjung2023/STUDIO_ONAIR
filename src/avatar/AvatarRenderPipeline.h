#pragma once

#include "avatar/AvatarMotionSample.h"
#include "avatar/AvatarParameterMapper.h"
#include "avatar/IAvatarRenderer.h"

namespace creator::avatar {

/// Maps one timestamped motion sample and submits it to the renderer adapter.
class AvatarRenderPipeline final {
public:
    AvatarRenderPipeline(const AvatarParameterMapper& mapper,
                         IAvatarRenderer& renderer)
        : mapper_(&mapper), renderer_(&renderer) {}

    [[nodiscard]] core::Result<AvatarRenderFrame> render(
        const AvatarMotionSample& sample) const;

private:
    const AvatarParameterMapper* mapper_;
    IAvatarRenderer* renderer_;
};

}  // namespace creator::avatar
