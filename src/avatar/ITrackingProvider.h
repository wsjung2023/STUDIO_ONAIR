#pragma once

#include "avatar/AvatarProviderId.h"
#include "avatar/TrackingResult.h"
#include "core/Result.h"
#include "media/MediaTypes.h"

namespace creator::avatar {

/// Port for a facial-expression tracking engine.
///
/// process() honestly takes a VideoFrame: a real implementation reads pixels.
/// This task ships no such implementation — only the port and its value types
/// (see the R3 plan's "fake ignores pixels" design decision) — so nothing here
/// assumes a particular pixel format or access path beyond what VideoFrame
/// already exposes.
///
/// Implementations own engine/model resources (a MediaPipe graph, GPU/CPU
/// inference buffers) and must never be copied or moved: copy/move are
/// deleted, matching ICaptureSource/IRecorder/IProjectStore in this codebase.
/// Held only through unique_ptr.
///
/// Adapters built on this port must not write to the project database
/// directly (CLAUDE.md 6, ARCHITECTURE.md 14): tracking output is recorded via
/// an application service, and persisted as avatar.motion telemetry, not
/// through this port.
class ITrackingProvider {
public:
    virtual ~ITrackingProvider() = default;

    ITrackingProvider(const ITrackingProvider&) = delete;
    ITrackingProvider& operator=(const ITrackingProvider&) = delete;
    ITrackingProvider(ITrackingProvider&&) = delete;
    ITrackingProvider& operator=(ITrackingProvider&&) = delete;

    [[nodiscard]] virtual AvatarProviderId providerId() const = 0;

    /// Fails with an AppError (never throws) if the engine cannot process the
    /// frame at all (e.g. an unsupported pixel format reaching a real
    /// implementation). A frame that is processed successfully but contains no
    /// face is not a failure: it is a successful Result whose TrackingResult
    /// has faceFound == false.
    [[nodiscard]] virtual core::Result<TrackingResult> process(const media::VideoFrame& frame) = 0;

protected:
    ITrackingProvider() = default;
};

}  // namespace creator::avatar
