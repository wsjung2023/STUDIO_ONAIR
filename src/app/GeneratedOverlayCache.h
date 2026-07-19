#pragma once

#include "core/AppError.h"
#include "core/Result.h"
#include "core/Timebase.h"
#include "domain/Timeline.h"
#include "edit_engine/EditEngineTypes.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <utility>
#include <vector>

namespace creator::app {

struct GeneratedOverlayCacheResult final {
    std::vector<edit_engine::GeneratedOverlayDescriptor> descriptors;
    std::vector<core::AppError> diagnostics;
};

class GeneratedOverlayCache final {
public:
    using BeforeCommitHook =
        std::function<core::Result<void>(const std::filesystem::path&)>;

    explicit GeneratedOverlayCache(BeforeCommitHook beforeCommit = {})
        : beforeCommit_(std::move(beforeCommit)) {}

    [[nodiscard]] core::Result<GeneratedOverlayCacheResult> synchronize(
        const std::filesystem::path& packageRoot,
        const domain::Timeline& timeline, std::int32_t canvasWidth,
        std::int32_t canvasHeight, core::FrameRate frameRate) const;

private:
    BeforeCommitHook beforeCommit_;
};

}  // namespace creator::app
