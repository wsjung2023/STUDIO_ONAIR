#include "avatar/AvatarMotionSerializer.h"

#include <nlohmann/json.hpp>

namespace creator::avatar {

nlohmann::json AvatarMotionSerializer::toJson(const AvatarMotionSample& sample) const {
    // parameters flattens the fixed ExpressionParameters struct to a
    // name->number map (struct->map, mirroring ProjectManifest->JSON): the
    // schema's `parameters` allows any number-valued keys, but this serializer
    // only ever emits exactly these nine documented fields.
    nlohmann::json parameters{
        {"eyeOpenLeft", sample.parameters.eyeOpenLeft},
        {"eyeOpenRight", sample.parameters.eyeOpenRight},
        {"browUpLeft", sample.parameters.browUpLeft},
        {"browUpRight", sample.parameters.browUpRight},
        {"mouthOpen", sample.parameters.mouthOpen},
        {"mouthWide", sample.parameters.mouthWide},
        {"headYaw", sample.parameters.headYaw},
        {"headPitch", sample.parameters.headPitch},
        {"headRoll", sample.parameters.headRoll},
    };

    nlohmann::json document;
    // See the class comment: intentionally not clamped when negative.
    document["tNs"] = sample.timestamp.time_since_epoch().count();
    document["type"] = kEventType;
    document["provider"] = sample.provider.value();
    document["parameters"] = std::move(parameters);
    return document;
}

std::string AvatarMotionSerializer::toNdjsonLine(const AvatarMotionSample& sample) const {
    return toJson(sample).dump() + '\n';
}

}  // namespace creator::avatar
