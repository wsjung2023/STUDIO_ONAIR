#include "avatar/openseeface/OpenSeeFaceTrackingProvider.h"

#include "avatar/PrimaryFaceSelector.h"
#include "avatar/openseeface/OpenSeeFaceUdpSource.h"
#include "core/AppError.h"

#include <utility>

namespace creator::avatar::openseeface {

OpenSeeFaceTrackingProvider::OpenSeeFaceTrackingProvider(
    std::unique_ptr<ITrackingSource> source, AvatarProviderId id)
    : source_(std::move(source)), id_(std::move(id)) {}

OpenSeeFaceTrackingProvider::~OpenSeeFaceTrackingProvider() {
    if (source_) source_->stop();
}

core::Result<std::unique_ptr<OpenSeeFaceTrackingProvider>>
OpenSeeFaceTrackingProvider::create(std::uint16_t port) {
    auto id = AvatarProviderId::create(kProviderId);
    if (!id.hasValue()) return id.error();

    auto source = std::make_unique<OpenSeeFaceUdpSource>();
    if (auto started = source->start(port); !started.hasValue()) {
        return started.error();
    }
    return std::unique_ptr<OpenSeeFaceTrackingProvider>(
        new OpenSeeFaceTrackingProvider(std::move(source), std::move(id).value()));
}

AvatarProviderId OpenSeeFaceTrackingProvider::providerId() const { return id_; }

std::uint16_t OpenSeeFaceTrackingProvider::boundPort() const noexcept {
    return source_ ? source_->boundPort() : 0;
}

core::Result<TrackingResult> OpenSeeFaceTrackingProvider::process(
    const media::VideoFrame& frame) {
    const auto projectTime = frame.timestamp;

    // Drain the socket to the newest datagram this tick: OpenSeeFace pushes at
    // its own ~30 fps and the avatar pulls at ~30 fps, so a backlog of more
    // than one datagram means we are behind — keep only the latest (preview
    // latest-frame strategy, CLAUDE.md 5).
    std::vector<TrackingResult> newest;
    bool gotDatagram = false;
    for (;;) {
        auto polled = source_->poll(projectTime);
        if (!polled.hasValue()) {
            // A malformed datagram is a recoverable per-packet error, not a
            // provider failure: surface it only if we have nothing else to
            // return this tick, otherwise keep the good batch we already have.
            if (!gotDatagram && !haveFace_) return polled.error();
            break;
        }
        if (polled.value().empty()) break;  // socket drained
        newest = std::move(polled).value();
        gotDatagram = true;
    }

    if (gotDatagram) {
        if (auto primary = selectPrimaryFace(newest); primary.has_value()) {
            lastFace_ = *primary;
            lastFace_.timestamp = projectTime;
            haveFace_ = true;
            return lastFace_;
        }
        // Datagram(s) arrived but no face was found in them. If we had a face
        // before, hold it (still preview frame); otherwise report no face.
    }

    if (haveFace_) {
        lastFace_.timestamp = projectTime;  // hold last pose, restamped
        return lastFace_;
    }

    // Never tracked a face yet: neutral, faceFound == false. Do not fabricate.
    TrackingResult none{};
    none.timestamp = projectTime;
    none.faceFound = false;
    none.confidence = 0.0F;
    return none;
}

}  // namespace creator::avatar::openseeface
