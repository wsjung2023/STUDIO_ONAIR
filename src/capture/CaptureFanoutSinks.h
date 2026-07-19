#pragma once

#include "capture/IAudioBlockSink.h"
#include "capture/IVideoFrameSink.h"

#include <atomic>
#include <memory>

namespace creator::capture {

/// Lock-free, dynamically attachable video fan-out for native callbacks.
///
/// The primary sink is the preview mailbox and remains attached for the source
/// lifetime. Recording owns the optional secondary sink and may atomically
/// attach or detach it without restarting capture.
class VideoFrameFanoutSink final : public IVideoFrameSink {
public:
    explicit VideoFrameFanoutSink(std::shared_ptr<IVideoFrameSink> primary);

    void setSecondary(std::shared_ptr<IVideoFrameSink> secondary) noexcept;

    void onCaptureStarted() noexcept override;
    void onVideoFrame(creator::media::VideoFrame frame) noexcept override;
    void onCaptureError(creator::core::AppError error) noexcept override;

private:
    std::shared_ptr<IVideoFrameSink> primary_;
    std::atomic<std::shared_ptr<IVideoFrameSink>> secondary_;
};

/// Audio counterpart of VideoFrameFanoutSink. The primary mailbox continues
/// to feed meters while the optional secondary sink feeds a recording worker.
class AudioBlockFanoutSink final : public IAudioBlockSink {
public:
    explicit AudioBlockFanoutSink(std::shared_ptr<IAudioBlockSink> primary);

    void setSecondary(std::shared_ptr<IAudioBlockSink> secondary) noexcept;

    void onCaptureStarted() noexcept override;
    void onAudioBlock(creator::media::AudioBlock block) noexcept override;
    void onCaptureError(creator::core::AppError error) noexcept override;

private:
    std::shared_ptr<IAudioBlockSink> primary_;
    std::atomic<std::shared_ptr<IAudioBlockSink>> secondary_;
};

}  // namespace creator::capture
