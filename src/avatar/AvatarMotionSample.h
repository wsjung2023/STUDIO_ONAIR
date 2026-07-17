#pragma once

#include "avatar/AvatarProviderId.h"
#include "avatar/ExpressionParameters.h"
#include "core/Timebase.h"

namespace creator::avatar {

/// One normalized expression reading, tagged with the project-timebase
/// timestamp it was produced at and the provider that produced it.
///
/// This is the value type that crosses from ExpressionNormalizer's output
/// into AvatarMotionSerializer, and from there into avatar.motion telemetry
/// (schemas/event.schema.json). Deliberately a plain aggregate: it has no
/// default constructor (AvatarProviderId has none — see domain::Identifier),
/// which is fine here since every consumer supplies a real provider id via
/// designated initialization rather than leaving one implicitly empty.
struct AvatarMotionSample final {
    core::TimestampNs timestamp;
    ExpressionParameters parameters;
    AvatarProviderId provider;
};

}  // namespace creator::avatar
