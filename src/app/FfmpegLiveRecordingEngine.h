#pragma once

#include "app/ILiveCaptureBindings.h"
#include "app/ILiveRecordingEngine.h"
#include "project_store/IProjectPackageStore.h"

#include <memory>
#include <mutex>
#include <string>

namespace creator::app {

class FfmpegLiveRecordingEngine final : public ILiveRecordingEngine {
public:
    struct Run;

    FfmpegLiveRecordingEngine(
        std::shared_ptr<ILiveCaptureBindings> captureBindings,
        std::shared_ptr<project_store::IProjectPackageStore> store);
    ~FfmpegLiveRecordingEngine() override;

    [[nodiscard]] bool available() const noexcept override;
    [[nodiscard]] std::string unavailableReason() const override;
    [[nodiscard]] core::Result<void> start(
        LiveRecordingStart start, Completion completion) override;
    [[nodiscard]] core::Result<std::vector<LiveCaptureSource>>
        sourceSnapshot() const override;
    void stopAsync(core::TimestampNs stoppedAt) override;
    [[nodiscard]] LiveRecordingEngineSnapshot snapshot() const override;

private:
    std::shared_ptr<ILiveCaptureBindings> captureBindings_;
    std::shared_ptr<project_store::IProjectPackageStore> store_;
    core::Result<void> capabilityStatus_{core::ok()};
    mutable std::mutex mutex_;
    std::shared_ptr<Run> run_;
};

}  // namespace creator::app
