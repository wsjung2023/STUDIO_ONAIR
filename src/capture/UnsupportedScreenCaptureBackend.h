#pragma once

#include "capture/IScreenCaptureDiscovery.h"
#include "capture/IScreenCapturePermission.h"
#include "capture/IScreenCaptureSourceFactory.h"

namespace creator::capture {

/// Explicit non-native fallback used on platforms that R0-03 does not target.
/// It never invents a device or silently shows synthetic frames as real input.
class UnsupportedScreenCapturePermission final : public IScreenCapturePermission {
public:
    [[nodiscard]] ScreenCapturePermissionStatus status() const noexcept override;
    void request(Completion completion) override;
};

class UnsupportedScreenCaptureDiscovery final : public IScreenCaptureDiscovery {
public:
    void enumerate(Completion completion) override;
};

class UnsupportedScreenCaptureSourceFactory final : public IScreenCaptureSourceFactory {
public:
    [[nodiscard]] creator::core::Result<std::unique_ptr<ICaptureSource>> create(
        const creator::domain::CaptureTargetId& targetId,
        std::shared_ptr<IVideoFrameSink> sink) override;
};

}  // namespace creator::capture

