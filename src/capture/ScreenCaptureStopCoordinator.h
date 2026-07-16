#pragma once

#include "capture/IScreenCaptureSource.h"
#include "core/AppError.h"
#include "core/Result.h"

#include <mutex>
#include <optional>
#include <vector>

namespace creator::capture {

/// Coalesces native stop callers and completes each callback exactly once.
class ScreenCaptureStopCoordinator final {
public:
    void add(IScreenCaptureSource::StopCompletion completion);
    void finish(creator::core::Result<void> result);

private:
    mutable std::mutex mutex_;
    std::vector<IScreenCaptureSource::StopCompletion> completions_;
    std::optional<creator::core::AppError> error_;
    bool completed_{false};
};

}  // namespace creator::capture
