#pragma once

#include "capture/ScreenCaptureTypes.h"
#include "core/Result.h"

#include <functional>
#include <vector>

namespace creator::capture {

/// Asynchronously enumerates the displays and windows visible to the adapter.
///
/// Implementations invoke completion exactly once with either a complete
/// immutable snapshot or one product error. The callback may arrive on an
/// adapter-owned thread; application code must marshal it to its own thread.
class IScreenCaptureDiscovery {
public:
    using Completion = std::function<void(
        creator::core::Result<std::vector<ScreenCaptureTarget>>)>;

    virtual ~IScreenCaptureDiscovery() = default;
    IScreenCaptureDiscovery(const IScreenCaptureDiscovery&) = delete;
    IScreenCaptureDiscovery& operator=(const IScreenCaptureDiscovery&) = delete;
    IScreenCaptureDiscovery(IScreenCaptureDiscovery&&) = delete;
    IScreenCaptureDiscovery& operator=(IScreenCaptureDiscovery&&) = delete;

    virtual void enumerate(Completion completion) = 0;

protected:
    IScreenCaptureDiscovery() = default;
};

}  // namespace creator::capture

