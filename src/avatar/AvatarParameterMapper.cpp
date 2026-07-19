#include "avatar/AvatarParameterMapper.h"

#include "core/AppError.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <unordered_set>

namespace creator::avatar {
namespace {

bool finite(float value) noexcept { return std::isfinite(value); }

float sourceValue(const ExpressionParameters& parameters,
                  AvatarParameterSource source) noexcept {
    switch (source) {
    case AvatarParameterSource::EyeOpenLeft: return parameters.eyeOpenLeft;
    case AvatarParameterSource::EyeOpenRight: return parameters.eyeOpenRight;
    case AvatarParameterSource::BrowUpLeft: return parameters.browUpLeft;
    case AvatarParameterSource::BrowUpRight: return parameters.browUpRight;
    case AvatarParameterSource::MouthOpen: return parameters.mouthOpen;
    case AvatarParameterSource::MouthWide: return parameters.mouthWide;
    case AvatarParameterSource::HeadYaw: return parameters.headYaw;
    case AvatarParameterSource::HeadPitch: return parameters.headPitch;
    case AvatarParameterSource::HeadRoll: return parameters.headRoll;
    }
    return 0.0F;
}

}  // namespace

core::Result<AvatarParameterMapper> AvatarParameterMapper::create(
    std::vector<AvatarParameterBinding> bindings) {
    std::unordered_set<std::string> names;
    names.reserve(bindings.size());
    for (const auto& binding : bindings) {
        if (binding.modelParameter.empty()) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "avatar model parameter name is empty"};
        }
        if (!finite(binding.scale) || !finite(binding.offset) ||
            !finite(binding.minimum) || !finite(binding.maximum) ||
            binding.minimum > binding.maximum) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "avatar parameter binding range is invalid"};
        }
        if (!names.insert(binding.modelParameter).second) {
            return core::AppError{core::ErrorCode::AlreadyExists,
                                  "avatar model parameter is mapped more than once"};
        }
    }
    return AvatarParameterMapper{std::move(bindings)};
}

core::Result<std::vector<AvatarParameterValue>> AvatarParameterMapper::map(
    const ExpressionParameters& parameters) const {
    std::vector<AvatarParameterValue> values;
    values.reserve(bindings_.size());
    for (const auto& binding : bindings_) {
        const float source = sourceValue(parameters, binding.source);
        const float mapped = source * binding.scale + binding.offset;
        if (!finite(source) || !finite(mapped)) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "avatar expression contains a non-finite value"};
        }
        values.push_back(AvatarParameterValue{
            binding.modelParameter,
            std::clamp(mapped, binding.minimum, binding.maximum)});
    }
    return values;
}

}  // namespace creator::avatar
