#include "avatar/AvatarModelDescriptor.h"

#include "core/AppError.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <limits>
#include <string_view>

namespace creator::avatar {
namespace {

using Json = nlohmann::json;

core::AppError invalid(std::string message) {
    return core::AppError{core::ErrorCode::InvalidArgument, std::move(message)};
}

core::Result<AvatarParameterSource> sourceFromJson(const Json& value) {
    if (!value.is_string()) return invalid("avatar parameter source is not a string");
    const auto source = value.get<std::string>();
    if (source == "eyeOpenLeft") return AvatarParameterSource::EyeOpenLeft;
    if (source == "eyeOpenRight") return AvatarParameterSource::EyeOpenRight;
    if (source == "browUpLeft") return AvatarParameterSource::BrowUpLeft;
    if (source == "browUpRight") return AvatarParameterSource::BrowUpRight;
    if (source == "mouthOpen") return AvatarParameterSource::MouthOpen;
    if (source == "mouthWide") return AvatarParameterSource::MouthWide;
    if (source == "headYaw") return AvatarParameterSource::HeadYaw;
    if (source == "headPitch") return AvatarParameterSource::HeadPitch;
    if (source == "headRoll") return AvatarParameterSource::HeadRoll;
    return invalid("avatar parameter source is unknown");
}

bool hasParentTraversal(const std::filesystem::path& relative) {
    for (const auto& component : relative) {
        if (component == "..") return true;
    }
    return false;
}

}  // namespace

core::Result<AvatarModelDescriptor> AvatarModelDescriptor::load(
    const std::filesystem::path& descriptorPath) {
    std::error_code error;
    const auto absoluteDescriptor = std::filesystem::absolute(descriptorPath, error);
    if (error || !std::filesystem::is_regular_file(absoluteDescriptor, error)) {
        return core::AppError{core::ErrorCode::NotFound,
                              "avatar descriptor file is not available"};
    }

    std::ifstream input{absoluteDescriptor, std::ios::binary};
    if (!input) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "avatar descriptor could not be opened"};
    }
    Json document;
    try {
        input >> document;
    } catch (const Json::exception&) {
        return core::AppError{core::ErrorCode::ParseFailure,
                              "avatar descriptor JSON is malformed"};
    }
    try {
        if (!document.is_object() || document.value("schemaVersion", 0) != 1) {
            return invalid("avatar descriptor schema version is unsupported");
        }
        const auto renderer = document.at("renderer").get<std::string>();
        const auto relativeModel =
            std::filesystem::path{document.at("model").get<std::string>()};
        if (renderer.empty() || relativeModel.empty() || relativeModel.is_absolute() ||
            hasParentTraversal(relativeModel)) {
            return invalid("avatar descriptor model path or renderer is invalid");
        }
        const auto canvas = document.at("canvas");
        const auto width = canvas.at("width").get<std::uint32_t>();
        const auto height = canvas.at("height").get<std::uint32_t>();
        constexpr std::uint32_t kMaximumCanvasDimension = 16'384;
        if (width == 0 || height == 0 || width > kMaximumCanvasDimension ||
            height > kMaximumCanvasDimension) {
            return invalid("avatar descriptor canvas dimensions are invalid");
        }

        const auto modelPath = absoluteDescriptor.parent_path() / relativeModel;
        if (!std::filesystem::is_regular_file(modelPath, error)) {
            return core::AppError{core::ErrorCode::NotFound,
                                  "avatar model file is not available"};
        }

        std::vector<AvatarParameterBinding> bindings;
        const auto parameterJson = document.value("parameters", Json::array());
        if (!parameterJson.is_array()) return invalid("avatar parameters is not an array");
        bindings.reserve(parameterJson.size());
        for (const auto& value : parameterJson) {
            const auto source = sourceFromJson(value.at("source"));
            if (!source.hasValue()) return source.error();
            AvatarParameterBinding binding{
                value.at("name").get<std::string>(), source.value(),
                value.value("scale", 1.0F), value.value("offset", 0.0F),
                value.value("minimum", 0.0F), value.value("maximum", 1.0F)};
            bindings.push_back(std::move(binding));
        }
        auto mapper = AvatarParameterMapper::create(std::move(bindings));
        if (!mapper.hasValue()) return mapper.error();
        return AvatarModelDescriptor{absoluteDescriptor, modelPath, renderer, width,
                                     height, std::move(mapper).value()};
    } catch (const Json::exception&) {
        return core::AppError{core::ErrorCode::ParseFailure,
                              "avatar descriptor is missing or has invalid fields"};
    }
}

}  // namespace creator::avatar
