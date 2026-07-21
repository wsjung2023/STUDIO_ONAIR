#include "capture/ScreenCaptureRegion.h"

#include "core/AppError.h"

#include <cstring>
#include <string>

namespace creator::capture {
namespace {

core::AppError invalid(std::string message) {
    return {core::ErrorCode::InvalidArgument, std::move(message)};
}

constexpr char kRegionSeparator[] = "#region=";

}  // namespace

core::Result<ScreenCaptureRegion> makeScreenCaptureRegion(std::int64_t x, std::int64_t y,
                                                          std::int64_t width,
                                                          std::int64_t height) {
    if (x < 0 || y < 0) {
        return invalid("capture region origin must not be negative");
    }
    if (width <= 0 || height <= 0) {
        return invalid("capture region must have a positive width and height");
    }
    constexpr std::int64_t kMax = 0xFFFFFFFF;
    if (x > kMax || y > kMax || width > kMax || height > kMax) {
        return invalid("capture region exceeds the addressable pixel range");
    }
    return ScreenCaptureRegion{.x = static_cast<std::uint32_t>(x),
                               .y = static_cast<std::uint32_t>(y),
                               .width = static_cast<std::uint32_t>(width),
                               .height = static_cast<std::uint32_t>(height)};
}

core::Result<void> ensureRegionWithinBounds(const ScreenCaptureRegion& region,
                                            std::uint32_t monitorWidth,
                                            std::uint32_t monitorHeight) {
    if (monitorWidth == 0 || monitorHeight == 0) {
        return invalid("monitor bounds are unknown for the capture region");
    }
    // Use 64-bit sums so x + width can never wrap past the monitor edge check.
    const std::uint64_t right = static_cast<std::uint64_t>(region.x) + region.width;
    const std::uint64_t bottom = static_cast<std::uint64_t>(region.y) + region.height;
    if (right > monitorWidth || bottom > monitorHeight) {
        return invalid("capture region lies outside the selected monitor");
    }
    return core::ok();
}

std::string encodeRegionTargetId(const std::string& baseId,
                                 const ScreenCaptureRegion& region) {
    std::string encoded = baseId;
    encoded += kRegionSeparator;
    encoded += std::to_string(region.x);
    encoded += ',';
    encoded += std::to_string(region.y);
    encoded += ',';
    encoded += std::to_string(region.width);
    encoded += ',';
    encoded += std::to_string(region.height);
    return encoded;
}

ParsedRegionTargetId parseRegionTargetId(const std::string& targetId) {
    const auto separatorPos = targetId.find(kRegionSeparator);
    if (separatorPos == std::string::npos) {
        return ParsedRegionTargetId{.baseId = targetId, .region = std::nullopt};
    }
    ParsedRegionTargetId parsed;
    parsed.baseId = targetId.substr(0, separatorPos);
    const auto fields =
        targetId.substr(separatorPos + std::strlen(kRegionSeparator));

    std::int64_t values[4] = {0, 0, 0, 0};
    std::size_t index = 0;
    std::size_t cursor = 0;
    while (index < 4) {
        const auto comma = fields.find(',', cursor);
        const auto token = fields.substr(
            cursor, comma == std::string::npos ? std::string::npos : comma - cursor);
        try {
            std::size_t consumed = 0;
            values[index] = std::stoll(token, &consumed);
            if (consumed != token.size()) {
                return ParsedRegionTargetId{.baseId = targetId, .region = std::nullopt};
            }
        } catch (...) {
            // A malformed region encoding is treated as no region rather than
            // throwing across the adapter boundary; the base id still resolves.
            return ParsedRegionTargetId{.baseId = targetId, .region = std::nullopt};
        }
        ++index;
        if (comma == std::string::npos) break;
        cursor = comma + 1;
    }
    if (index != 4) {
        return ParsedRegionTargetId{.baseId = targetId, .region = std::nullopt};
    }
    auto region = makeScreenCaptureRegion(values[0], values[1], values[2], values[3]);
    if (!region.hasValue()) {
        return ParsedRegionTargetId{.baseId = targetId, .region = std::nullopt};
    }
    parsed.region = region.value();
    return parsed;
}

core::Result<CroppedBgra8> cropBgra8(const std::uint8_t* pixels, std::uint32_t srcWidth,
                                     std::uint32_t srcHeight,
                                     const ScreenCaptureRegion& region) {
    if (pixels == nullptr) {
        return invalid("cropBgra8 received a null source buffer");
    }
    if (region.width == 0 || region.height == 0) {
        return invalid("cropBgra8 received an empty region");
    }
    auto bounded = ensureRegionWithinBounds(region, srcWidth, srcHeight);
    if (!bounded.hasValue()) {
        return bounded.error();
    }

    const std::size_t srcStride = static_cast<std::size_t>(srcWidth) * 4U;
    const std::size_t dstStride = static_cast<std::size_t>(region.width) * 4U;
    CroppedBgra8 cropped;
    cropped.width = region.width;
    cropped.height = region.height;
    cropped.pixels.resize(dstStride * region.height);
    for (std::uint32_t row = 0; row < region.height; ++row) {
        const std::uint8_t* src = pixels +
                                  static_cast<std::size_t>(region.y + row) * srcStride +
                                  static_cast<std::size_t>(region.x) * 4U;
        std::memcpy(cropped.pixels.data() + static_cast<std::size_t>(row) * dstStride, src,
                    dstStride);
    }
    return cropped;
}

}  // namespace creator::capture
