#include "avatar/AvatarRenderFrame.h"

#include "core/AppError.h"

#include <limits>

namespace creator::avatar {
namespace {

core::Result<std::uint32_t> checkedStride(std::uint32_t width,
                                          std::uint32_t stride) {
    constexpr std::uint64_t kBytesPerPixel = 4;
    const auto minimum = static_cast<std::uint64_t>(width) * kBytesPerPixel;
    if (stride < minimum || stride > std::numeric_limits<std::uint32_t>::max()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "avatar BGRA stride is invalid"};
    }
    return stride;
}

core::Result<std::uint32_t> packedStride(std::uint32_t width) {
    const auto stride = static_cast<std::uint64_t>(width) * 4U;
    if (stride > std::numeric_limits<std::uint32_t>::max()) {
        return core::AppError{core::ErrorCode::InsufficientStorage,
                              "avatar BGRA stride overflows frame metadata"};
    }
    return static_cast<std::uint32_t>(stride);
}

core::Result<std::size_t> checkedBytes(std::uint32_t height, std::uint32_t stride) {
    const auto bytes = static_cast<std::uint64_t>(height) * stride;
    if (bytes > std::numeric_limits<std::size_t>::max()) {
        return core::AppError{core::ErrorCode::InsufficientStorage,
                              "avatar BGRA frame size overflows memory size"};
    }
    return static_cast<std::size_t>(bytes);
}

}  // namespace

core::Result<AvatarRenderFrame> AvatarRenderFrame::transparent(
    core::TimestampNs timestamp, std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "transparent avatar frame dimensions are empty"};
    }
    const auto packed = packedStride(width);
    if (!packed.hasValue()) return packed.error();
    const auto stride = checkedStride(width, packed.value());
    if (!stride.hasValue()) return stride.error();
    const auto bytes = checkedBytes(height, stride.value());
    if (!bytes.hasValue()) return bytes.error();
    auto storage = std::make_shared<const std::vector<std::uint8_t>>(
        bytes.value(), static_cast<std::uint8_t>(0));
    return AvatarRenderFrame{timestamp, width, height, stride.value(),
                             std::move(storage)};
}

core::Result<AvatarRenderFrame> AvatarRenderFrame::fromBgra(
    core::TimestampNs timestamp, std::uint32_t width, std::uint32_t height,
    std::uint32_t stride, std::vector<std::uint8_t> bytes) {
    if (width == 0 || height == 0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "avatar BGRA frame dimensions are empty"};
    }
    const auto checked = checkedStride(width, stride);
    if (!checked.hasValue()) return checked.error();
    const auto expected = checkedBytes(height, stride);
    if (!expected.hasValue()) return expected.error();
    if (bytes.size() != expected.value()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "avatar BGRA byte count does not match stride"};
    }
    auto storage = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
    return AvatarRenderFrame{timestamp, width, height, stride, std::move(storage)};
}

std::span<const std::uint8_t> AvatarRenderFrame::bytes() const noexcept {
    return bytes_ ? std::span<const std::uint8_t>{*bytes_}
                  : std::span<const std::uint8_t>{};
}

}  // namespace creator::avatar
