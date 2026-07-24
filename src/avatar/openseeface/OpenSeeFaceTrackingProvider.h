#pragma once

#include "avatar/AvatarProviderId.h"
#include "avatar/ITrackingProvider.h"
#include "avatar/ITrackingSource.h"
#include "avatar/TrackingResult.h"
#include "avatar/openseeface/OpenSeeFaceParser.h"
#include "core/Result.h"

#include <cstdint>
#include <memory>

namespace creator::avatar::openseeface {

/// Adapts the out-of-process, self-capturing OpenSeeFace UDP tracker to the
/// in-process, frame-pull ITrackingProvider port the live avatar scene
/// (AvatarSceneController) consumes.
///
/// OpenSeeFace owns the camera and streams results asynchronously over UDP;
/// this app only receives them. ITrackingProvider::process() is a pull: each
/// avatar tick hands us a frame whose ONLY meaningful field here is its
/// project-timebase timestamp (pixels are ignored — OpenSeeFace already saw
/// the camera). On each process() we drain the socket to the newest datagram
/// (latest-frame preview strategy, sanctioned by CLAUDE.md 5), pick the
/// primary face with selectPrimaryFace, and return it stamped at the frame's
/// project time.
///
/// When no datagram arrived this tick we hold the last received face (a still
/// preview frame, again latest-frame), so a 30 fps UDP feed under a 30 fps
/// avatar tick does not flicker to neutral between packets. Before any face is
/// ever seen, process() returns a neutral faceFound == false result rather
/// than a fabricated pose (CLAUDE.md 9): the caller must not treat "not yet
/// tracking" as a real expression.
class OpenSeeFaceTrackingProvider final : public ITrackingProvider {
public:
    static constexpr const char* kProviderId = "openseeface-udp";

    /// Binds the UDP receive socket on `port` (OpenSeeFace's default 11573).
    /// Fails with the socket error if the port cannot be bound (e.g. already
    /// held by another receiver) — a bind failure is surfaced, never hidden.
    [[nodiscard]] static core::Result<std::unique_ptr<OpenSeeFaceTrackingProvider>>
    create(std::uint16_t port = kDefaultUdpPort);

    ~OpenSeeFaceTrackingProvider() override;

    [[nodiscard]] AvatarProviderId providerId() const override;

    /// Drains all pending datagrams, selecting the primary face from the newest
    /// non-empty batch, and returns it timestamped at frame.timestamp. Never
    /// throws; a malformed datagram from the source surfaces as its AppError.
    [[nodiscard]] core::Result<TrackingResult> process(
        const media::VideoFrame& frame) override;

    /// True once at least one faceFound datagram has been received and is being
    /// held. Lets the UI/harness report whether the real tracker has locked on.
    [[nodiscard]] bool hasFace() const noexcept { return haveFace_; }
    [[nodiscard]] std::uint16_t boundPort() const noexcept;

private:
    OpenSeeFaceTrackingProvider(std::unique_ptr<ITrackingSource> source,
                                AvatarProviderId id);

    std::unique_ptr<ITrackingSource> source_;
    AvatarProviderId id_;
    TrackingResult lastFace_{};
    bool haveFace_{false};
};

}  // namespace creator::avatar::openseeface
