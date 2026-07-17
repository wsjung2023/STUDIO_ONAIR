#include "app/LiveRecordingEngineFactory.h"

#include "app/ControllerLiveCaptureBindings.h"
#if defined(CS_APP_ENABLE_FFMPEG)
#include "app/FfmpegLiveRecordingEngine.h"
#endif

#include <utility>

namespace creator::app {
namespace {

#if !defined(CS_APP_ENABLE_FFMPEG)
class UnavailableLiveRecordingEngine final : public ILiveRecordingEngine {
public:
    [[nodiscard]] bool available() const noexcept override { return false; }
    [[nodiscard]] std::string unavailableReason() const override {
        return "Audited FFmpeg recording runtime is not installed in this build";
    }
    [[nodiscard]] core::Result<void> start(LiveRecordingStart,
                                            Completion) override {
        return core::AppError{core::ErrorCode::UnsupportedVersion,
                              unavailableReason()};
    }
    [[nodiscard]] core::Result<std::vector<LiveCaptureSource>>
    sourceSnapshot() const override {
        return core::AppError{core::ErrorCode::UnsupportedVersion,
                              unavailableReason()};
    }
    void stopAsync(core::TimestampNs) override {}
    [[nodiscard]] LiveRecordingEngineSnapshot snapshot() const override {
        return {};
    }
};
#endif

}  // namespace

std::unique_ptr<ILiveRecordingEngine> makeLiveRecordingEngine(
    ScreenCaptureController* screen, DeviceCaptureController* devices,
    std::shared_ptr<project_store::IProjectPackageStore> store) {
#if defined(CS_APP_ENABLE_FFMPEG)
    return std::make_unique<FfmpegLiveRecordingEngine>(
        std::make_shared<ControllerLiveCaptureBindings>(screen, devices),
        std::move(store));
#else
    static_cast<void>(screen);
    static_cast<void>(devices);
    static_cast<void>(store);
    return std::make_unique<UnavailableLiveRecordingEngine>();
#endif
}

}  // namespace creator::app
