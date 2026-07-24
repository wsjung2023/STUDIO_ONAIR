#include "app/VerticalLayout.h"

#include <utility>

namespace creator::app {
namespace {

// A 16:9 source fit to the full portrait width occupies this fraction of the
// portrait height: (canvasW * 9/16) / canvasH = for 1080x1920, 607.5/1920.
constexpr double kVideoBandHeight = 0.316;
// Screen sits just below a small top margin; camera fills a lower band, leaving
// breathing room between them and at the bottom.
constexpr double kScreenTopY = 0.04;
constexpr double kCameraTopY = 0.60;
// Avatar: small picture-in-picture in the lower-right corner, above the video.
constexpr double kAvatarX = 0.62;
constexpr double kAvatarY = 0.66;
constexpr double kAvatarWidth = 0.36;
constexpr double kAvatarHeight = 0.30;
constexpr std::int32_t kVideoZ = 0;
constexpr std::int32_t kAvatarZ = 20;

std::optional<domain::VisualTransform> box(double x, double y, double width,
                                           double height, std::int32_t z) {
    auto t = domain::VisualTransform::create(x, y, width, height, 1.0, 1.0, 0.0,
                                             0.0, 0.0, 0.0, 0.0, 1.0, z);
    if (!t.hasValue()) return std::nullopt;
    return std::move(t).value();
}

}  // namespace

std::optional<domain::VisualTransform> verticalDefaultTransform(
    domain::StudioSourceRole role, std::int32_t canvasWidth,
    std::int32_t canvasHeight) {
    if (canvasHeight <= canvasWidth) return std::nullopt;  // landscape: no override
    switch (role) {
        case domain::StudioSourceRole::Screen:
            return box(0.0, kScreenTopY, 1.0, kVideoBandHeight, kVideoZ);
        case domain::StudioSourceRole::Camera:
            return box(0.0, kCameraTopY, 1.0, kVideoBandHeight, kVideoZ);
        case domain::StudioSourceRole::Avatar:
            return box(kAvatarX, kAvatarY, kAvatarWidth, kAvatarHeight, kAvatarZ);
        case domain::StudioSourceRole::Microphone:
        case domain::StudioSourceRole::SystemAudio:
            return std::nullopt;  // audio roles carry no visual transform
    }
    return std::nullopt;
}

}  // namespace creator::app
