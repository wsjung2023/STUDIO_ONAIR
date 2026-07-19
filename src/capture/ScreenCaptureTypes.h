#pragma once

#include "core/Result.h"
#include "domain/Identifiers.h"

#include <cstdint>
#include <optional>
#include <string>

namespace creator::capture {

enum class ScreenCaptureTargetKind { Display, Window };

/// A display or window visible during one discovery session.
///
/// Native display/window objects and numeric OS ids never leave the adapter.
/// The opaque id is valid only for selecting a target from the associated
/// discovery result and is not durable project identity.
class ScreenCaptureTarget final {
public:
    [[nodiscard]] static creator::core::Result<ScreenCaptureTarget> create(
        creator::domain::CaptureTargetId id, ScreenCaptureTargetKind kind,
        std::string displayName, std::optional<std::string> applicationName,
        std::uint32_t width, std::uint32_t height);

    [[nodiscard]] const creator::domain::CaptureTargetId& id() const noexcept { return id_; }
    [[nodiscard]] ScreenCaptureTargetKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::string& displayName() const noexcept { return displayName_; }
    [[nodiscard]] const std::optional<std::string>& applicationName() const noexcept {
        return applicationName_;
    }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

    friend bool operator==(const ScreenCaptureTarget&, const ScreenCaptureTarget&) = default;

private:
    ScreenCaptureTarget(creator::domain::CaptureTargetId id, ScreenCaptureTargetKind kind,
                        std::string displayName,
                        std::optional<std::string> applicationName, std::uint32_t width,
                        std::uint32_t height) noexcept;

    creator::domain::CaptureTargetId id_;
    ScreenCaptureTargetKind kind_;
    std::string displayName_;
    std::optional<std::string> applicationName_;
    std::uint32_t width_;
    std::uint32_t height_;
};

enum class ScreenCapturePermissionStatus { Unknown, Granted, Denied };

}  // namespace creator::capture

