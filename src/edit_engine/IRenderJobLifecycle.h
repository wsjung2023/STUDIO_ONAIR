#pragma once

#include "core/Result.h"
#include "domain/Identifiers.h"
#include "edit_engine/EditEngineTypes.h"

#include <filesystem>
#include <string>

namespace creator::edit_engine {

struct RenderEncoderDiagnostics final {
    std::string attemptedEncoders;
    std::string selectedEncoder;
    std::string fallbackReason;
};

/// Synchronous durability boundary for a render job. Implementations must not
/// return from preparePublication until enough evidence has been persisted to
/// recover or reject the exact partial artifact after a process crash.
class IRenderJobLifecycle {
public:
    [[nodiscard]] virtual core::Result<void> begin(
        const RenderRequest& request, const std::filesystem::path& partial,
        core::DurationNs totalDuration) = 0;
    [[nodiscard]] virtual core::Result<void> encoderSelected(
        const domain::RenderJobId& jobId,
        const RenderEncoderDiagnostics& diagnostics) = 0;
    [[nodiscard]] virtual core::Result<void> advance(
        const domain::RenderJobId& jobId, const RenderProgress& progress) = 0;
    [[nodiscard]] virtual core::Result<void> preparePublication(
        const domain::RenderJobId& jobId,
        const std::filesystem::path& partial,
        const RenderProgress& progress) = 0;
    [[nodiscard]] virtual core::Result<void> finish(
        const domain::RenderJobId& jobId, RenderJobState state,
        std::string diagnostic) = 0;
    virtual ~IRenderJobLifecycle() = default;
};

}  // namespace creator::edit_engine
