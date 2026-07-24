#pragma once

#include "avatar/AvatarProviderId.h"
#include "avatar/ITrackingProvider.h"
#include "avatar/TrackingResult.h"
#include "core/Result.h"
#include "media/MediaTypes.h"

namespace creator::avatar {

/// A deterministic, in-process ITrackingProvider that SYNTHESIZES a moving
/// expression purely from the incoming frame's timestamp.
///
/// This is explicitly NOT a face tracker: it reads no pixels (honouring the
/// same "the fake ignores pixels" contract as FakeTrackingProvider) and no
/// real face-tracking model runs behind it. It exists so the end-to-end
/// tracking -> parameter-map -> render -> composite -> record chain can be
/// proven with a webcam present but without a MediaPipe/OpenSeeFace model
/// available on this machine. The application drives it from the live camera's
/// presence and the project timebase, so the avatar only animates while the
/// webcam is actually capturing.
///
/// Output is a continuous, periodic function of the frame timestamp (blink,
/// mouth, brow and head-sway cycles), so identical timestamps always produce
/// identical readings. faceFound is always true and confidence is always 1.0:
/// the synthesized pose is, by construction, a fully-known "expression".
///
/// Callers MUST surface this as synthetic/fake tracking in the UI and logs
/// (CLAUDE.md 9 forbids hiding what is really happening).
class SyntheticFaceTrackingProvider final : public ITrackingProvider {
public:
    SyntheticFaceTrackingProvider();

    [[nodiscard]] AvatarProviderId providerId() const override;
    [[nodiscard]] core::Result<TrackingResult> process(
        const media::VideoFrame& frame) override;

    /// The provider identity string, exposed so tests and telemetry can assert
    /// the fake origin without duplicating the literal.
    static constexpr const char* kProviderId = "synthetic-fake-tracker";

private:
    AvatarProviderId id_;
};

}  // namespace creator::avatar
