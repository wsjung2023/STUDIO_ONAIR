#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "edit_engine/EditEngineTypes.h"

#include <QByteArray>

#include <cstddef>
#include <filesystem>

namespace creator::app::android {

struct AndroidTimelineExportPlan final {
    QByteArray json;
    core::DurationNs duration;
    std::size_t visualClipCount{};
    std::size_t audioClipCount{};
};

/// Freezes every enabled timeline consumer into the platform-render manifest.
/// Missing/offline inputs are errors so Android can never report a successful
/// export that silently omitted media or generated text.
[[nodiscard]] core::Result<AndroidTimelineExportPlan>
buildAndroidTimelineExportPlan(
    const edit_engine::RenderRequest& request,
    const std::filesystem::path& partialDestination);

}  // namespace creator::app::android
