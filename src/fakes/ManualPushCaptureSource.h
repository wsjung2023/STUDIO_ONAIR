#pragma once

#include "capture/IScreenCaptureSource.h"
#include "capture/IVideoFrameSink.h"
#include "core/Result.h"
#include "media/MediaTypes.h"

#include <memory>
#include <string>

namespace creator::fakes {

/// Deterministic push source used to test application orchestration without a
/// native capture API, worker thread, timer, or sleep.
class ManualPushCaptureSource final : public creator::capture::IScreenCaptureSource {
public:
    ManualPushCaptureSource(creator::domain::SourceId id, std::string displayName,
                            std::shared_ptr<creator::capture::IVideoFrameSink> sink);

    [[nodiscard]] creator::domain::SourceId id() const override;
    [[nodiscard]] std::string displayName() const override;
    [[nodiscard]] creator::core::Result<void> start(
        const creator::capture::CaptureConfig& config) override;
    [[nodiscard]] creator::core::Result<void> stop() override;
    void stopAsync(StopCompletion completion) override;
    [[nodiscard]] creator::capture::CaptureStats stats() const noexcept override;

    [[nodiscard]] creator::core::Result<void> pushFrame(
        creator::media::VideoFrame frame);
    [[nodiscard]] creator::core::Result<void> fail(creator::core::AppError error);

private:
    creator::domain::SourceId id_;
    std::string displayName_;
    std::shared_ptr<creator::capture::IVideoFrameSink> sink_;
    creator::capture::CaptureStats stats_;
    bool started_{false};
};

}  // namespace creator::fakes
