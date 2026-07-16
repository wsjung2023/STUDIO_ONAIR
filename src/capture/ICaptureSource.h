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
/// droppedFrames counts native dropped-frame markers reported by the source;
/// it must not include normal idle frames, malformed input, or preview mailbox
/// replacement. "녹화 실패를 숨기지 않는다" (CLAUDE.md 9) means each category
/// reaches the UI rather than being folded into a reassuring zero.
struct CaptureStats final {
    std::uint64_t receivedFrames{0};
    std::uint64_t droppedFrames{0};
    std::uint64_t ignoredFrames{0};
    std::uint64_t invalidFrames{0};
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
/// Push implementations constructor-inject IVideoFrameSink; pull test sources
/// derive through IPullCaptureSource. Delivery stays off this lifecycle port so
/// platform callback types cannot leak across the boundary.
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

    /// Must be safe to call on a source that was never started and immediately
    /// bar further frame callbacks. Purely synchronous sources release all
    /// handles before return. Native screen sources additionally expose
    /// IScreenCaptureSource::stopAsync so their OS completion and error remain
    /// observable without blocking the UI thread.
    [[nodiscard]] virtual creator::core::Result<void> stop() = 0;

    [[nodiscard]] virtual CaptureStats stats() const noexcept = 0;

protected:
    ICaptureSource() = default;
};

}  // namespace creator::capture
