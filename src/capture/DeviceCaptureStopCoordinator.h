#pragma once

#include "capture/IDeviceCaptureSource.h"
#include "core/AppError.h"
#include "core/Result.h"

#include <mutex>
#include <optional>
#include <vector>

namespace creator::capture {

/// Thread-safe exactly-once fanout for one native camera/audio stop operation.
class DeviceCaptureStopCoordinator final {
public:
    void add(IDeviceCaptureSource::StopCompletion completion);
    void finish(creator::core::Result<void> result);

private:
    std::mutex mutex_;
    std::vector<IDeviceCaptureSource::StopCompletion> completions_;
    std::optional<creator::core::AppError> error_;
    bool completed_{false};
};

}  // namespace creator::capture
