#pragma once

#include "autozoom/ZoomCandidate.h"
#include "core/Result.h"
#include "core/Timebase.h"
#include "cursor_emphasis/ClickEmphasis.h"
#include "mlt_adapter/FrameEffects.h"

#include <cstdint>
#include <vector>

namespace creator::mlt_adapter {

/// Accepted, editable R2-02/R2-03 visual directives for one rendered stream.
/// Cursor hiding is intentionally not represented as a destructive pixel
/// operation: removing a cursor requires a clean plate or a replacement asset,
/// which the capture/editor layer must provide explicitly.
struct CursorVisualEffectsPlan final {
    std::vector<cursor_emphasis::ClickEmphasis> clicks;
    std::vector<autozoom::ZoomCandidate> zooms;
};

/// Applies the active zoom viewport and click emphasis at one project timestamp.
/// The returned frame owns its pixels whenever an effect is active and otherwise
/// aliases the validated source frame.
[[nodiscard]] core::Result<ProcessedBgraFrame> applyCursorVisualEffects(
    BgraFrameView source, std::uint32_t canvasWidth, std::uint32_t canvasHeight,
    core::TimestampNs position, const CursorVisualEffectsPlan& plan);

}  // namespace creator::mlt_adapter

