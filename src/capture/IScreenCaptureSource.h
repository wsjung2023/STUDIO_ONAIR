#pragma once

#include "capture/ICaptureSource.h"
#include "core/Result.h"

#include <functional>

namespace creator::capture {

/// A screen source whose native teardown may complete asynchronously.
///
/// The completion is invoked exactly once. After stopAsync() is called, frame
/// and error callbacks are barred immediately; native resources stay alive
/// until the completion reports the final stop result.
class IScreenCaptureSource : public ICaptureSource {
public:
    using StopCompletion = std::function<void(creator::core::Result<void>)>;

    ~IScreenCaptureSource() override = default;

    virtual void stopAsync(StopCompletion completion) = 0;

protected:
    IScreenCaptureSource() = default;
};

}  // namespace creator::capture
