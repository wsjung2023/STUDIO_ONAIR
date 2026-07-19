#include "avatar/AvatarMotionPlaybackRenderer.h"

#include "core/AppError.h"

namespace creator::avatar {

core::Result<media::VideoFrame> AvatarMotionPlaybackRenderer::renderAt(
    core::TimestampNs timestamp) const {
    if (playback_ == nullptr || renderer_ == nullptr) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "avatar playback renderer is unavailable"};
    }
    auto sample = playback_->sampleAt(timestamp);
    if (!sample.hasValue()) return sample.error();
    auto rendered = renderer_->render(sample.value());
    if (!rendered.hasValue()) return rendered.error();
    return rendered.value().toVideoFrame();
}

}  // namespace creator::avatar
