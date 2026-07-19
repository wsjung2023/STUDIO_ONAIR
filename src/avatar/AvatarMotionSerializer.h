#pragma once

#include "avatar/AvatarMotionSample.h"

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace creator::avatar {

/// Turns an AvatarMotionSample into the `avatar.motion` telemetry event shape
/// defined by the `avatarMotion` branch of schemas/event.schema.json:
/// `{tNs, type:"avatar.motion", provider, parameters:{<field>:<number>...}}`.
/// The schema is the authority on that shape, not this class — see
/// AvatarMotionSerializerTest, which validates this serializer's actual
/// output against schemas/event.schema.json with the in-tree
/// json-schema-validator rather than merely asserting language-level facts
/// about the struct.
///
/// Contract on `tNs`: the schema requires `tNs >= 0`. This class does not
/// clamp a negative `sample.timestamp` to 0 — doing so would silently turn a
/// caller bug (a timestamp that should never go negative on the steady
/// project timebase, per core::ProjectClock) into a plausible-looking event,
/// which CLAUDE.md 9 forbids ("오류를 조용히 무시하지 않는다"). Instead a
/// negative timestamp is serialized as-is and the resulting document fails
/// schema validation, so the error surfaces loudly at the validation
/// boundary instead of being hidden here. Every real producer (the fake
/// tracking provider, and any real one later) sources its timestamp from a
/// captured frame on the project timebase, which never goes negative.
class AvatarMotionSerializer final {
public:
    /// The `type` discriminator for every event this serializer produces.
    static constexpr const char* kEventType = "avatar.motion";

    [[nodiscard]] nlohmann::json toJson(const AvatarMotionSample& sample) const;

    /// Compact single-line JSON followed by exactly one '\n', ready to append
    /// to an NDJSON file. Never embeds a newline: dump() with no indent
    /// argument produces none, and this appends only the one trailing
    /// separator.
    [[nodiscard]] std::string toNdjsonLine(const AvatarMotionSample& sample) const;
};

}  // namespace creator::avatar
