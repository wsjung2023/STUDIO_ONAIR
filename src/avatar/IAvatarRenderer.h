#pragma once

#include "avatar/AvatarParameterMapper.h"
#include "avatar/AvatarRenderFrame.h"
#include "core/Result.h"

#include <span>

namespace creator::avatar {

/// Renderer adapter boundary for Inochi2D or another avatar engine.
class IAvatarRenderer {
public:
    virtual ~IAvatarRenderer() = default;
    IAvatarRenderer(const IAvatarRenderer&) = delete;
    IAvatarRenderer& operator=(const IAvatarRenderer&) = delete;
    IAvatarRenderer(IAvatarRenderer&&) = delete;
    IAvatarRenderer& operator=(IAvatarRenderer&&) = delete;

    [[nodiscard]] virtual core::Result<AvatarRenderFrame> render(
        core::TimestampNs timestamp,
        std::span<const AvatarParameterValue> parameters) = 0;

protected:
    IAvatarRenderer() = default;
};

}  // namespace creator::avatar
