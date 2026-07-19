#pragma once

#include "capture/ICaptureSource.h"

#include <functional>

namespace creator::capture {

/// Camera or audio source with asynchronously completed native teardown.
class IDeviceCaptureSource : public ICaptureSource {
public:
    using StopCompletion = std::function<void(creator::core::Result<void>)>;

    ~IDeviceCaptureSource() override = default;
    virtual void stopAsync(StopCompletion completion) = 0;

protected:
    IDeviceCaptureSource() = default;
};

}  // namespace creator::capture
