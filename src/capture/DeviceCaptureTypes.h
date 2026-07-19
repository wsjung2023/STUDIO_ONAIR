#pragma once

#include "core/Result.h"
#include "domain/Identifiers.h"

#include <string>

namespace creator::capture {

enum class CaptureDeviceKind { Camera, Microphone };

enum class MediaPermissionStatus { Unknown, Granted, Denied, Restricted };

/// One immutable device from a native discovery snapshot.
class CaptureDeviceInfo final {
public:
    [[nodiscard]] static creator::core::Result<CaptureDeviceInfo> create(
        creator::domain::CaptureDeviceId id, CaptureDeviceKind kind,
        std::string displayName, bool isDefault);

    [[nodiscard]] const creator::domain::CaptureDeviceId& id() const noexcept {
        return id_;
    }
    [[nodiscard]] CaptureDeviceKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::string& displayName() const noexcept { return displayName_; }
    [[nodiscard]] bool isDefault() const noexcept { return isDefault_; }

    friend bool operator==(const CaptureDeviceInfo&, const CaptureDeviceInfo&) = default;

private:
    CaptureDeviceInfo(creator::domain::CaptureDeviceId id, CaptureDeviceKind kind,
                      std::string displayName, bool isDefault) noexcept;

    creator::domain::CaptureDeviceId id_;
    CaptureDeviceKind kind_;
    std::string displayName_;
    bool isDefault_;
};

}  // namespace creator::capture
