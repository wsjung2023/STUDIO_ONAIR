#pragma once

#include "domain/Identifiers.h"

namespace creator::avatar {

struct AvatarProviderTag;
/// Typed identity for a tracking engine (e.g. "mediapipe", "openseeface", the
/// deterministic fake) — "매직 문자열 대신 typed ID/value object 사용"
/// (CLAUDE.md 4) applies to provider identity the same way it does to source
/// and project ids.
///
/// Split into its own header rather than living in ITrackingProvider.h: plain
/// data types that only need the id (AvatarMotionSample, its serializer)
/// would otherwise have to include the whole port — and with it
/// media::VideoFrame — just to name a provider. ITrackingProvider.h includes
/// this header so nothing that already used AvatarProviderId/AvatarProviderTag
/// from there needs to change.
using AvatarProviderId = domain::Identifier<AvatarProviderTag>;

}  // namespace creator::avatar
