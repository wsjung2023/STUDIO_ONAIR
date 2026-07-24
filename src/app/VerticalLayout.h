#pragma once

#include "domain/StudioScene.h"    // StudioSourceRole
#include "domain/TimelineTypes.h"  // VisualTransform

#include <cstdint>
#include <optional>

namespace creator::app {

/// Default composition transform for a recorded (landscape) source on a PORTRAIT
/// (9:16) canvas: screen fit-to-width at the top, camera as a lower full-width
/// band, avatar as a lower-corner picture-in-picture. Coordinates are normalized
/// [0,1] of the canvas (matching domain::VisualTransform).
///
/// Returns std::nullopt for a landscape canvas (canvasHeight <= canvasWidth) or a
/// non-video role, so the caller keeps the recorded scene's own transform. This
/// is only the DEFAULT layout the shorts project imports with — the editor can
/// override each clip's transform afterward.
[[nodiscard]] std::optional<domain::VisualTransform> verticalDefaultTransform(
    domain::StudioSourceRole role, std::int32_t canvasWidth,
    std::int32_t canvasHeight);

}  // namespace creator::app
