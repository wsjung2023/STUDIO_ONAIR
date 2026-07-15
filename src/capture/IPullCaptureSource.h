#pragma once

#include "capture/ICaptureSource.h"
#include "core/Result.h"
#include "media/MediaTypes.h"

namespace creator::capture {

/// A capture source that produces frames on demand instead of pushing them.
///
/// Real OS capture pushes: ScreenCaptureKit and Windows.Graphics.Capture call
/// us when a frame is ready. But some sources genuinely pull - a file-backed
/// source, a generated test pattern - and the application has to drive those
/// from a timer.
///
/// This is a separate port rather than a tick() bolted onto ICaptureSource,
/// because a push source has no meaning for tick() and should not be forced to
/// implement it. The alternative - one interface plus a dynamic_cast in the
/// application layer - would put a boundary violation in the very commit that
/// establishes the boundary.
class IPullCaptureSource : public ICaptureSource {
public:
    /// Produces the next frame. Fails with InvalidState unless started.
    ///
    /// The caller decides when frames appear, which is what makes a pull source
    /// testable without threads or sleeping (CLAUDE.md 9 bans sleep-based sync).
    [[nodiscard]] virtual creator::core::Result<creator::media::VideoFrame> tick() = 0;

protected:
    IPullCaptureSource() = default;
};

}  // namespace creator::capture
