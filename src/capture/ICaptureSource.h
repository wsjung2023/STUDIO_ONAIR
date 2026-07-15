#pragma once

#include "core/Result.h"
#include "domain/Identifiers.h"

#include <cstdint>
#include <string>

namespace creator::capture {

/// What a capture source is asked to produce. The source may not be able to
/// honour it exactly (a display has the resolution it has), so it reports what
/// it actually delivers through stats() rather than silently pretending.
struct CaptureConfig final {
    std::uint32_t targetWidth{1920};
    std::uint32_t targetHeight{1080};
    std::uint32_t frameRateNumerator{60};
    std::uint32_t frameRateDenominator{1};
};

/// Live counters for the diagnostics surface.
///
/// droppedFrames is not an internal detail: "녹화 실패를 숨기지 않는다"
/// (CLAUDE.md 9) means this reaches the UI while recording, not just the log.
struct CaptureStats final {
    std::uint64_t receivedFrames{0};
    std::uint64_t droppedFrames{0};
    double currentFps{0.0};
};

/// A source of frames: a display, a window, a region, a camera, a microphone or
/// system audio.
///
/// Signature follows ARCHITECTURE.md 4.3. start() returns Result rather than
/// bool because bool throws away why it failed, and the reasons here are ones
/// the user has to act on: permission denied, the captured window closed, the
/// device was unplugged.
///
/// Implementations must never write to the project database. Recording what was
/// captured goes through an application service (CLAUDE.md 6); a source that
/// reaches into the DB makes the capture layer untestable and couples it to
/// storage.
///
/// Frame delivery is deliberately not on this interface yet. Real OS capture
/// pushes - ScreenCaptureKit and Windows.Graphics.Capture call us when a frame
/// is ready - so a push source needs some way to hand frames onward, and this
/// port does not define one. IPullCaptureSource covers the pull path, which is
/// all R0-01 needs.
///
/// The shape is left open on purpose rather than guessed at: designing a
/// callback without the real SCK/WGC APIs in front of us would be speculation,
/// and R0-01 is explicitly barred from implementing either. R0-03 decides it,
/// with two candidates already visible - constructor-injecting a sink into the
/// concrete source (keeps the port minimal), or adding sink registration here
/// (makes delivery part of the contract). Whoever picks: the layers above must
/// not have to know which kind of source they hold.
class ICaptureSource {
public:
    virtual ~ICaptureSource() = default;

    ICaptureSource(const ICaptureSource&) = delete;
    ICaptureSource& operator=(const ICaptureSource&) = delete;
    ICaptureSource(ICaptureSource&&) = delete;
    ICaptureSource& operator=(ICaptureSource&&) = delete;

    [[nodiscard]] virtual creator::domain::SourceId id() const = 0;
    [[nodiscard]] virtual std::string displayName() const = 0;

    [[nodiscard]] virtual creator::core::Result<void> start(const CaptureConfig& config) = 0;

    /// Must be safe to call on a source that was never started, and must release
    /// every OS handle it holds (CLAUDE.md 8 requires resource cleanup to be
    /// verified, so this has to be idempotent to be testable).
    [[nodiscard]] virtual creator::core::Result<void> stop() = 0;

    [[nodiscard]] virtual CaptureStats stats() const noexcept = 0;

protected:
    ICaptureSource() = default;
};

}  // namespace creator::capture
