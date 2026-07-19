#include "capture/ScreenCaptureStopCoordinator.h"

#include <utility>

namespace creator::capture {

void ScreenCaptureStopCoordinator::add(IScreenCaptureSource::StopCompletion completion) {
    std::optional<core::AppError> completedError;
    {
        std::scoped_lock lock{mutex_};
        if (!completed_) {
            completions_.push_back(std::move(completion));
            return;
        }
        completedError = error_;
    }
    if (!completion) return;
    completion(completedError ? core::Result<void>{*completedError} : core::ok());
}

void ScreenCaptureStopCoordinator::finish(core::Result<void> result) {
    std::vector<IScreenCaptureSource::StopCompletion> completions;
    std::optional<core::AppError> error;
    {
        std::scoped_lock lock{mutex_};
        if (completed_) return;
        completed_ = true;
        if (!result.hasValue()) error_ = result.error();
        error = error_;
        completions = std::move(completions_);
    }
    for (auto& completion : completions) {
        if (!completion) continue;
        completion(error ? core::Result<void>{*error} : core::ok());
    }
}

}  // namespace creator::capture
