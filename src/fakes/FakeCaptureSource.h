#pragma once

#include "capture/IPullCaptureSource.h"
#include "core/Result.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "media/MediaTypes.h"

#include <cstdint>
#include <optional>
#include <string>

namespace creator::fakes {

/// An IPullCaptureSource that emits timestamps and nothing else.
///
/// Deliberately has no thread and never reads a clock. Frames appear only when
/// tick() is called, and each timestamp is computed from the frame index via
/// frameToTimestamp(), so a 60fps source produces exactly 1/60s spacing and
/// frame 60 lands exactly one second in. A fake that spawned a thread and slept
/// would violate the "sleep() 기반 동기화" ban in CLAUDE.md 9, and would make
/// every test that used it slow and flaky.
///
/// Frames carry no pixels: platformHandle stays null. This exists to prove the
/// port and the timing contract before ScreenCaptureKit and
/// Windows.Graphics.Capture land in R0-03, not to draw anything.
class FakeCaptureSource final : public creator::capture::IPullCaptureSource {
public:
    FakeCaptureSource(creator::domain::SourceId id, std::string displayName);

    [[nodiscard]] creator::domain::SourceId id() const override;
    [[nodiscard]] std::string displayName() const override;

    /// Fails with InvalidState if already started, or InvalidArgument if the
    /// config's frame rate is not a valid rational.
    [[nodiscard]] creator::core::Result<void> start(
        const creator::capture::CaptureConfig& config) override;

    /// Safe to call when never started, and safe to call twice.
    [[nodiscard]] creator::core::Result<void> stop() override;

    [[nodiscard]] creator::capture::CaptureStats stats() const noexcept override;

    /// Produces the next frame. Fails with InvalidState unless started.
    [[nodiscard]] creator::core::Result<creator::media::VideoFrame> tick() override;

    /// Makes the next start() call fail with `error` instead of taking effect,
    /// then reverts to normal behaviour. No real capture device is available
    /// to fail on demand, so StudioController's failure paths - the ones
    /// CLAUDE.md 8 requires an error-path test for - are otherwise
    /// unreachable from a test. Deliberately minimal: one pending error, no
    /// policy engine.
    void failNextStart(creator::core::AppError error);

    /// Same as failNextStart, but for the next stop() call.
    void failNextStop(creator::core::AppError error);

private:
    creator::domain::SourceId id_;
    std::string displayName_;
    bool started_{false};
    creator::capture::CaptureConfig config_{};
    std::optional<creator::core::FrameRate> frameRate_;
    std::int64_t nextFrameIndex_{0};
    std::optional<creator::core::AppError> failNextStart_;
    std::optional<creator::core::AppError> failNextStop_;
};

}  // namespace creator::fakes
