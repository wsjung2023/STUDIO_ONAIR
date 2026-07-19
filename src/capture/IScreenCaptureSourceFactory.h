#pragma once

#include "capture/IScreenCaptureSource.h"
#include "capture/IVideoFrameSink.h"
#include "core/Result.h"
#include "domain/Identifiers.h"

#include <memory>

namespace creator::capture {

/// Creates a target-specific push source from the adapter's latest discovery
/// snapshot. The opaque target id is resolved inside the platform adapter;
/// native display/window objects never cross this boundary.
class IScreenCaptureSourceFactory {
public:
    virtual ~IScreenCaptureSourceFactory() = default;
    IScreenCaptureSourceFactory(const IScreenCaptureSourceFactory&) = delete;
    IScreenCaptureSourceFactory& operator=(const IScreenCaptureSourceFactory&) = delete;
    IScreenCaptureSourceFactory(IScreenCaptureSourceFactory&&) = delete;
    IScreenCaptureSourceFactory& operator=(IScreenCaptureSourceFactory&&) = delete;

    [[nodiscard]] virtual creator::core::Result<std::unique_ptr<IScreenCaptureSource>> create(
        const creator::domain::CaptureTargetId& targetId,
        std::shared_ptr<IVideoFrameSink> sink) = 0;

protected:
    IScreenCaptureSourceFactory() = default;
};

}  // namespace creator::capture
