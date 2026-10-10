#pragma once

#include "avatar/AvatarProviderId.h"
#include "avatar/ITrackingProvider.h"
#include "avatar/TrackingResult.h"
#include "core/Result.h"
#include "core/Timebase.h"
#include "media/MediaTypes.h"

#include <atomic>
#include <optional>

namespace creator::avatar {

/// Low-load avatar driver: the mouth follows the microphone level, the eyes
/// blink on their own and the head breathes/nods -- no camera, no model.
///
/// This is the "voice only" tier (the same idea as a video-meeting avatar that
/// lip-syncs from audio when face tracking is off): honest about being audio
/// driven (providerId "audio-lipsync"; callers label it so), but unlike the
/// synthetic tracker it reacts to the creator -- when they talk the mouth
/// moves, when they stop it closes.
///
/// Timing comes only from the processed frame's timestamp (project timebase,
/// never a wall clock). The audio level is pushed in from whatever thread
/// measures it (an atomic), so this stays Qt-free and lock-free.
class AudioLipSyncTrackingProvider final : public ITrackingProvider {
public:
    AudioLipSyncTrackingProvider();

    [[nodiscard]] AvatarProviderId providerId() const override;
    [[nodiscard]] core::Result<TrackingResult> process(
        const media::VideoFrame& frame) override;

    /// Latest microphone RMS level in dBFS (silence <= kSilenceGateDbfs).
    /// Thread-safe; the next processed frame picks it up.
    void setAudioLevelDbfs(float rmsDbfs) noexcept;

    static constexpr const char* kProviderId = "audio-lipsync";
    /// Below this the mouth is shut (room noise / mic hiss must not flutter it).
    static constexpr float kSilenceGateDbfs = -50.0F;
    /// At or above this the mouth is fully open; levels in between scale linearly in dB.
    static constexpr float kFullOpenDbfs = -18.0F;
    /// Mouth attack / release time constants: open fast, close a little slower.
    static constexpr float kAttackSeconds = 0.035F;
    static constexpr float kReleaseSeconds = 0.11F;
    /// Blink every ~3.6 s, closed for ~130 ms (eased in/out).
    static constexpr float kBlinkPeriodSeconds = 3.6F;
    static constexpr float kBlinkDurationSeconds = 0.13F;

private:
    AvatarProviderId id_;
    std::atomic<float> levelDbfs_{-120.0F};
    float mouth_{0.0F};
    std::optional<core::TimestampNs> lastTimestamp_;
};

}  // namespace creator::avatar
