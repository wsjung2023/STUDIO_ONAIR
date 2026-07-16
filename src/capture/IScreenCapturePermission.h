#pragma once

#include "capture/ScreenCaptureTypes.h"
#include "core/Result.h"

#include <functional>

namespace creator::capture {

/// Screen-recording permission boundary.
///
/// status() is a non-prompting snapshot. request() is invoked only after the UI
/// has explained why capture is required and completes exactly once. Native
/// permission types and settings URLs remain inside the platform adapter.
class IScreenCapturePermission {
public:
    using Completion =
        std::function<void(creator::core::Result<ScreenCapturePermissionStatus>)>;

    virtual ~IScreenCapturePermission() = default;
    IScreenCapturePermission(const IScreenCapturePermission&) = delete;
    IScreenCapturePermission& operator=(const IScreenCapturePermission&) = delete;
    IScreenCapturePermission(IScreenCapturePermission&&) = delete;
    IScreenCapturePermission& operator=(IScreenCapturePermission&&) = delete;

    [[nodiscard]] virtual ScreenCapturePermissionStatus status() const noexcept = 0;
    virtual void request(Completion completion) = 0;

protected:
    IScreenCapturePermission() = default;
};

}  // namespace creator::capture

