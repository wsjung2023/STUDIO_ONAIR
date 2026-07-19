#pragma once

#include "avatar/AvatarMotionPlayback.h"
#include "avatar/AvatarRenderPipeline.h"
#include "core/Result.h"
#include "media/MediaTypes.h"

namespace creator::avatar {

/// Connects editor-time motion playback to the same renderer as live motion.
class AvatarMotionPlaybackRenderer final {
public:
    AvatarMotionPlaybackRenderer(const AvatarMotionPlayback& playback,
                                 const AvatarRenderPipeline& renderer)
        : playback_(&playback), renderer_(&renderer) {}

    [[nodiscard]] core::Result<media::VideoFrame> renderAt(
        core::TimestampNs timestamp) const;

private:
    const AvatarMotionPlayback* playback_;
    const AvatarRenderPipeline* renderer_;
};

}  // namespace creator::avatar
