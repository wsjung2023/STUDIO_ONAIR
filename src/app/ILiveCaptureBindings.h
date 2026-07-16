#pragma once

#include "capture/IAudioBlockSink.h"
#include "capture/IVideoFrameSink.h"
#include "core/Result.h"
#include "recorder/RecordingTrack.h"

#include <functional>
#include <memory>
#include <vector>

namespace creator::app {

struct LiveCaptureSource final {
    domain::SourceId sourceId;
    recorder::TrackRole role;
};

/// Discovers active capture controllers and attaches recording-only sinks.
/// Preview and meters remain owned by their controllers.
class ILiveCaptureBindings {
public:
    virtual ~ILiveCaptureBindings() = default;
    ILiveCaptureBindings(const ILiveCaptureBindings&) = delete;
    ILiveCaptureBindings& operator=(const ILiveCaptureBindings&) = delete;

    [[nodiscard]] virtual std::vector<LiveCaptureSource> activeSources() const = 0;
    [[nodiscard]] virtual core::Result<void> attach(
        const LiveCaptureSource& source,
        std::shared_ptr<capture::IVideoFrameSink> videoSink,
        std::shared_ptr<capture::IAudioBlockSink> audioSink) = 0;
    virtual void detachAll() noexcept = 0;
    virtual void dispatch(std::function<void()> work) = 0;

protected:
    ILiveCaptureBindings() = default;
};

}  // namespace creator::app
