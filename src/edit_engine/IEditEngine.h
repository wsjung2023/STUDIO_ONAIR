#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "edit_engine/EditEngineTypes.h"

#include <memory>
#include <string>

namespace creator::edit_engine {

class IRenderJob {
public:
    [[nodiscard]] virtual core::Result<RenderProgress> progress() const = 0;
    [[nodiscard]] virtual core::Result<void> cancel() = 0;
    [[nodiscard]] virtual std::string diagnostic() const { return {}; }
    virtual ~IRenderJob() = default;
};

class IEditEngine {
public:
    [[nodiscard]] virtual core::Result<void> load(
        const TimelineSnapshot& snapshot) = 0;
    [[nodiscard]] virtual core::Result<void> update(
        const TimelineChangeSet& change) = 0;
    [[nodiscard]] virtual core::Result<void> play() = 0;
    [[nodiscard]] virtual core::Result<void> pause() = 0;
    [[nodiscard]] virtual core::Result<void> seek(core::TimestampNs position) = 0;
    [[nodiscard]] virtual core::Result<PreviewFrame> requestFrame(
        core::TimestampNs position) = 0;
    [[nodiscard]] virtual core::Result<std::unique_ptr<IRenderJob>> render(
        const RenderRequest& request) = 0;
    virtual ~IEditEngine() = default;
};

}  // namespace creator::edit_engine
