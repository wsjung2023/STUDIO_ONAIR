#pragma once

#include "app/ILiveCaptureBindings.h"

namespace creator::app {

class AvatarSceneController;
class DeviceCaptureController;
class ScreenCaptureController;

class ControllerLiveCaptureBindings final : public ILiveCaptureBindings {
public:
    ControllerLiveCaptureBindings(ScreenCaptureController* screen,
                                  DeviceCaptureController* devices,
                                  AvatarSceneController* avatar = nullptr);

    [[nodiscard]] std::vector<LiveCaptureSource> activeSources() const override;
    [[nodiscard]] core::Result<void> attach(
        const LiveCaptureSource& source,
        std::shared_ptr<capture::IVideoFrameSink> videoSink,
        std::shared_ptr<capture::IAudioBlockSink> audioSink) override;
    void detachAll() noexcept override;
    void dispatch(std::function<void()> work) override;

private:
    ScreenCaptureController* screen_{};
    DeviceCaptureController* devices_{};
    AvatarSceneController* avatar_{};
};

}  // namespace creator::app
