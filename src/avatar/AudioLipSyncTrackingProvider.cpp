#include "avatar/AudioLipSyncTrackingProvider.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace creator::avatar {
namespace {

constexpr float kPi = 3.14159265358979323846F;

float clampUnit(float value) noexcept { return std::clamp(value, 0.0F, 1.0F); }
float clampBipolar(float value) noexcept { return std::clamp(value, -1.0F, 1.0F); }

}  // namespace

AudioLipSyncTrackingProvider::AudioLipSyncTrackingProvider()
    : id_(AvatarProviderId::create(kProviderId).value()) {}

AvatarProviderId AudioLipSyncTrackingProvider::providerId() const { return id_; }

void AudioLipSyncTrackingProvider::setAudioLevelDbfs(float rmsDbfs) noexcept {
    levelDbfs_.store(std::isfinite(rmsDbfs) ? rmsDbfs : -120.0F,
                     std::memory_order_relaxed);
}

core::Result<TrackingResult> AudioLipSyncTrackingProvider::process(
    const media::VideoFrame& frame) {
    const float t = std::chrono::duration<float>(frame.timestamp.time_since_epoch()).count();
    // Frame spacing drives the mouth smoothing; the first frame (or a jump
    // backwards after a seek) uses a nominal 1/30 s.
    float dt = 1.0F / 30.0F;
    if (lastTimestamp_ && frame.timestamp > *lastTimestamp_) {
        dt = std::clamp(
            std::chrono::duration<float>(frame.timestamp - *lastTimestamp_).count(),
            0.001F, 0.25F);
    }
    lastTimestamp_ = frame.timestamp;

    // ---- Mouth from the microphone level ----
    const float level = levelDbfs_.load(std::memory_order_relaxed);
    const float target =
        clampUnit((level - kSilenceGateDbfs) / (kFullOpenDbfs - kSilenceGateDbfs));
    const float tau = target > mouth_ ? kAttackSeconds : kReleaseSeconds;
    const float alpha = 1.0F - std::exp(-dt / tau);
    mouth_ += (target - mouth_) * alpha;
    mouth_ = clampUnit(mouth_);

    // ---- Blink: deterministic in the timebase, both eyes together ----
    const float blinkPhase = std::fmod(t + 1.1F, kBlinkPeriodSeconds);
    float eyeOpen = 1.0F;
    if (blinkPhase >= 0.0F && blinkPhase < kBlinkDurationSeconds) {
        eyeOpen = 1.0F - std::sin((blinkPhase / kBlinkDurationSeconds) * kPi);
    }

    // ---- Idle life: breathing pitch, slow sway, a small nod while talking ----
    const float breathe = 0.035F * std::sin(2.0F * kPi * t / 4.2F);
    const float sway = 0.045F * std::sin(2.0F * kPi * t / 7.1F);
    const float roll = 0.025F * std::sin(2.0F * kPi * t / 5.3F + 0.8F);
    const float nod = 0.06F * mouth_ * std::sin(2.0F * kPi * t * 1.8F);

    ExpressionParameters parameters;
    parameters.eyeOpenLeft = clampUnit(eyeOpen);
    parameters.eyeOpenRight = clampUnit(eyeOpen);
    parameters.browUpLeft = clampUnit(0.15F + 0.2F * mouth_);
    parameters.browUpRight = parameters.browUpLeft;
    parameters.mouthOpen = mouth_;
    parameters.mouthWide = clampUnit(0.15F + 0.35F * mouth_);
    parameters.headYaw = clampBipolar(sway);
    parameters.headPitch = clampBipolar(breathe + nod);
    parameters.headRoll = clampBipolar(roll);

    return TrackingResult{.timestamp = frame.timestamp,
                          .raw = parameters,
                          .confidence = 1.0F,
                          .faceFound = true};
}

}  // namespace creator::avatar
