#include "avatar/SyntheticFaceTrackingProvider.h"

#include "core/AppError.h"

#include <algorithm>
#include <cmath>

namespace creator::avatar {
namespace {

constexpr float kPi = 3.14159265358979323846F;

float clampUnit(float value) noexcept { return std::clamp(value, 0.0F, 1.0F); }
float clampBipolar(float value) noexcept { return std::clamp(value, -1.0F, 1.0F); }

}  // namespace

SyntheticFaceTrackingProvider::SyntheticFaceTrackingProvider()
    : id_(AvatarProviderId::create(kProviderId).value()) {}

AvatarProviderId SyntheticFaceTrackingProvider::providerId() const { return id_; }

core::Result<TrackingResult> SyntheticFaceTrackingProvider::process(
    const media::VideoFrame& frame) {
    // Seconds on the project timebase. The application feeds a small,
    // monotonically increasing value (elapsed since the avatar started), which
    // keeps the trigonometric arguments well-conditioned.
    const auto seconds =
        std::chrono::duration<double>(frame.timestamp.time_since_epoch()).count();
    const auto t = static_cast<float>(seconds);

    ExpressionParameters parameters;

    // Blink: eyes stay open, dipping briefly to nearly closed on a ~3.1 s
    // cadence. The dip is a single sine lobe so the eye closes and reopens.
    constexpr float kBlinkPeriod = 3.1F;
    constexpr float kBlinkDuration = 0.22F;
    const float blinkPhase = std::fmod(std::fmod(t, kBlinkPeriod) + kBlinkPeriod,
                                       kBlinkPeriod);
    float eyeOpen = 1.0F;
    if (blinkPhase < kBlinkDuration) {
        eyeOpen = 1.0F - std::sin((blinkPhase / kBlinkDuration) * kPi);
    }
    eyeOpen = clampUnit(std::max(eyeOpen, 0.05F));
    parameters.eyeOpenLeft = eyeOpen;
    parameters.eyeOpenRight = eyeOpen;

    // Mouth: a "talking" open/close oscillation with a little shaping so it is
    // clearly, visibly moving.
    const float mouth = 0.5F * (1.0F - std::cos(t * 4.4F));
    parameters.mouthOpen = clampUnit(0.1F + 0.85F * std::pow(mouth, 1.3F));
    parameters.mouthWide = clampUnit(0.2F + 0.25F * (0.5F * (1.0F + std::sin(t * 1.3F))));

    // Brows: gentle raise/lower.
    const float brow = clampUnit(0.15F + 0.3F * (0.5F * (1.0F + std::sin(t * 0.9F))));
    parameters.browUpLeft = brow;
    parameters.browUpRight = brow;

    // Head sway: yaw/pitch/roll on distinct slow periods so the head visibly
    // turns, nods, and tilts.
    parameters.headYaw = clampBipolar(0.35F * std::sin(t * 0.55F));
    parameters.headPitch = clampBipolar(0.18F * std::sin(t * 0.37F + 1.0F));
    parameters.headRoll = clampBipolar(0.15F * std::sin(t * 0.70F + 0.5F));

    TrackingResult result;
    result.timestamp = frame.timestamp;
    result.raw = parameters;
    result.confidence = 1.0F;
    result.faceFound = true;
    return result;
}

}  // namespace creator::avatar
