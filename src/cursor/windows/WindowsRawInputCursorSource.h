#pragma once

#include "core/AppError.h"
#include "core/Result.h"
#include "cursor/ICursorSource.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace creator::cursor::windows {

/// Physical-pixel rectangle represented by the cursor telemetry source.
///
/// The default (zero-sized) rectangle asks the factory to use the Windows
/// virtual desktop. A caller recording one display can provide that display's
/// physical bounds instead, keeping normalization tied to the same geometry as
/// the screen capture source.
struct CursorCaptureGeometry final {
    std::int32_t left{};
    std::int32_t top{};
    std::uint32_t width{};
    std::uint32_t height{};
};

/// Windows Raw Input cursor source.
///
/// The source owns a message-only window on a dedicated thread. It registers
/// RIDEV_INPUTSINK for mouse input, samples GetPhysicalCursorPos at each input
/// packet, and exposes only neutral RawCursorSample values through ICursorSource.
/// No window handles or Windows message types cross the cursor module boundary.
class WindowsRawInputCursorSource final : public ICursorSource {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<WindowsRawInputCursorSource>>
    create(CursorCaptureGeometry geometry = {}, std::size_t queueCapacity = 8192);

    ~WindowsRawInputCursorSource() override;

    WindowsRawInputCursorSource(const WindowsRawInputCursorSource&) = delete;
    WindowsRawInputCursorSource& operator=(const WindowsRawInputCursorSource&) = delete;
    WindowsRawInputCursorSource(WindowsRawInputCursorSource&&) = delete;
    WindowsRawInputCursorSource& operator=(WindowsRawInputCursorSource&&) = delete;

    [[nodiscard]] std::optional<RawCursorSample> poll() override;

    /// Returns a startup/runtime error observed by the message thread, if any.
    [[nodiscard]] std::optional<core::AppError> error() const;

    /// Number of oldest samples discarded when the bounded queue was full.
    [[nodiscard]] std::uint64_t droppedSamples() const noexcept;

private:
    WindowsRawInputCursorSource(CursorCaptureGeometry geometry,
                                 std::size_t queueCapacity);

    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace creator::cursor::windows

