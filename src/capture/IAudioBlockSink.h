#pragma once

#include "core/AppError.h"
#include "media/MediaTypes.h"

namespace creator::capture {

/// Fast, exception-free destination for ordered native audio callbacks.
class IAudioBlockSink {
public:
    virtual ~IAudioBlockSink() = default;
    IAudioBlockSink(const IAudioBlockSink&) = delete;
    IAudioBlockSink& operator=(const IAudioBlockSink&) = delete;
    IAudioBlockSink(IAudioBlockSink&&) = delete;
    IAudioBlockSink& operator=(IAudioBlockSink&&) = delete;

    virtual void onCaptureStarted() noexcept = 0;
    virtual void onAudioBlock(creator::media::AudioBlock block) noexcept = 0;
    virtual void onCaptureError(creator::core::AppError error) noexcept = 0;

protected:
    IAudioBlockSink() = default;
};

}  // namespace creator::capture
