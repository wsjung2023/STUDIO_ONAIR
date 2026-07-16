#pragma once

#include "core/AppError.h"
#include "media/MediaTypes.h"

namespace creator::capture {

/// Thread-boundary destination for a push video source.
///
/// Source callbacks are serialized. Implementations must return quickly and
/// must not throw: a native capture callback queue cannot safely unwind through
/// Objective-C or OS code. A source delivers at most one terminal error, and
/// its stop() call is a barrier after which no callbacks are made.
class IVideoFrameSink {
public:
    virtual ~IVideoFrameSink() = default;
    IVideoFrameSink(const IVideoFrameSink&) = delete;
    IVideoFrameSink& operator=(const IVideoFrameSink&) = delete;
    IVideoFrameSink(IVideoFrameSink&&) = delete;
    IVideoFrameSink& operator=(IVideoFrameSink&&) = delete;

    virtual void onVideoFrame(creator::media::VideoFrame frame) noexcept = 0;
    virtual void onCaptureError(creator::core::AppError error) noexcept = 0;

protected:
    IVideoFrameSink() = default;
};

}  // namespace creator::capture

