#pragma once

#include "avatar/AvatarProviderId.h"
#include "avatar/ExpressionParameters.h"
#include "avatar/ITrackingProvider.h"
#include "avatar/TrackingResult.h"
#include "core/Result.h"
#include "media/MediaTypes.h"

#include <cstddef>
#include <vector>

namespace creator::fakes {

/// An ITrackingProvider that emits a scripted sequence of expression readings
/// and nothing else.
///
/// Mirrors FakeCaptureSource's discipline exactly: deliberately reads no
/// clock, never sleeps, spawns no thread, and never looks at the frame's
/// pixels (platformHandle stays whatever the caller passed in, including
/// null - see the R3 plan's "the fake ignores pixels" design decision). The
/// only part of the incoming VideoFrame this class ever reads is `timestamp`,
/// which becomes the returned TrackingResult's timestamp. Every other field -
/// raw expression, confidence, faceFound - comes solely from the script
/// supplied at construction, so identical scripts fed identical frames
/// produce byte-identical output on every run.
class FakeTrackingProvider final : public avatar::ITrackingProvider {
public:
    /// One scripted process() outcome: the raw expression a real engine would
    /// have reported, its confidence, and whether it found a face at all.
    /// Deliberately excludes a timestamp - process() always takes that from
    /// the frame it is given, never from the script.
    struct ScriptedFrame final {
        avatar::ExpressionParameters parameters{};
        float confidence{1.0F};
        bool faceFound{true};
    };

    /// `script` must not be empty - a provider with nothing to say is a
    /// configuration mistake, not a valid "no face" reading (that is
    /// ScriptedFrame{.faceFound = false}, an explicit script entry). `id`
    /// defaults to "fake-tracker" so most call sites do not have to name one.
    explicit FakeTrackingProvider(std::vector<ScriptedFrame> script,
                                   avatar::AvatarProviderId id = defaultProviderId());

    [[nodiscard]] avatar::AvatarProviderId providerId() const override;

    /// Returns the next scripted entry, in order, tagged with `frame`'s own
    /// timestamp. After the script is exhausted, repeats its last entry
    /// (clamps rather than cycles): a caller that overruns the script gets a
    /// stable, obviously-scripted repeat instead of silently wrapping back
    /// into an unrelated earlier frame's data.
    [[nodiscard]] core::Result<avatar::TrackingResult> process(
        const media::VideoFrame& frame) override;

private:
    [[nodiscard]] static avatar::AvatarProviderId defaultProviderId();

    std::vector<ScriptedFrame> script_;
    avatar::AvatarProviderId id_;
    std::size_t nextIndex_{0};
};

}  // namespace creator::fakes
