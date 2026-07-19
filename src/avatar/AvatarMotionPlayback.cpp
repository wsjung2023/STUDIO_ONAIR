#include "avatar/AvatarMotionPlayback.h"

#include "core/AppError.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <string>

namespace creator::avatar {
namespace {

using Json = nlohmann::json;

core::AppError parseError(const char* message) {
    return core::AppError{core::ErrorCode::ParseFailure, message};
}

core::Result<float> number(const Json& object, const char* key) {
    const auto iterator = object.find(key);
    if (iterator == object.end() || !iterator->is_number()) {
        return parseError("avatar motion parameter is missing or not numeric");
    }
    const auto value = iterator->get<float>();
    if (!std::isfinite(value)) return parseError("avatar motion parameter is not finite");
    return value;
}

core::Result<AvatarMotionSample> parseSample(const Json& document) {
    try {
        if (!document.is_object() || document.value("type", "") != "avatar.motion") {
            return parseError("avatar telemetry event type is invalid");
        }
        const auto timestamp = document.at("tNs").get<std::int64_t>();
        if (timestamp < 0) return parseError("avatar telemetry timestamp is negative");
        const auto provider = AvatarProviderId::create(
            document.at("provider").get<std::string>());
        if (!provider.hasValue()) return provider.error();
        const auto& parameters = document.at("parameters");
        if (!parameters.is_object()) return parseError("avatar telemetry parameters are invalid");
        const auto read = [&parameters](const char* key) {
            return number(parameters, key);
        };
        auto eyeOpenLeft = read("eyeOpenLeft");
        auto eyeOpenRight = read("eyeOpenRight");
        auto browUpLeft = read("browUpLeft");
        auto browUpRight = read("browUpRight");
        auto mouthOpen = read("mouthOpen");
        auto mouthWide = read("mouthWide");
        auto headYaw = read("headYaw");
        auto headPitch = read("headPitch");
        auto headRoll = read("headRoll");
        for (const auto* value : {&eyeOpenLeft, &eyeOpenRight, &browUpLeft,
                                  &browUpRight, &mouthOpen, &mouthWide,
                                  &headYaw, &headPitch, &headRoll}) {
            if (!value->hasValue()) return value->error();
        }
        ExpressionParameters values{
            .eyeOpenLeft = eyeOpenLeft.value(),
            .eyeOpenRight = eyeOpenRight.value(),
            .browUpLeft = browUpLeft.value(),
            .browUpRight = browUpRight.value(),
            .mouthOpen = mouthOpen.value(),
            .mouthWide = mouthWide.value(),
            .headYaw = headYaw.value(),
            .headPitch = headPitch.value(),
            .headRoll = headRoll.value()};
        return AvatarMotionSample{core::TimestampNs{core::DurationNs{timestamp}},
                                   values, std::move(provider).value()};
    } catch (const Json::exception&) {
        return parseError("avatar telemetry event fields are invalid");
    }
}

ExpressionParameters interpolate(const ExpressionParameters& first,
                                  const ExpressionParameters& second,
                                  float ratio) noexcept {
    const auto blend = [ratio](float left, float right) {
        return left + (right - left) * ratio;
    };
    return ExpressionParameters{
        .eyeOpenLeft = blend(first.eyeOpenLeft, second.eyeOpenLeft),
        .eyeOpenRight = blend(first.eyeOpenRight, second.eyeOpenRight),
        .browUpLeft = blend(first.browUpLeft, second.browUpLeft),
        .browUpRight = blend(first.browUpRight, second.browUpRight),
        .mouthOpen = blend(first.mouthOpen, second.mouthOpen),
        .mouthWide = blend(first.mouthWide, second.mouthWide),
        .headYaw = blend(first.headYaw, second.headYaw),
        .headPitch = blend(first.headPitch, second.headPitch),
        .headRoll = blend(first.headRoll, second.headRoll)};
}

}  // namespace

core::Result<AvatarMotionPlayback> AvatarMotionPlayback::load(
    const std::filesystem::path& telemetryPath) {
    std::ifstream input{telemetryPath, std::ios::binary};
    if (!input) {
        return core::AppError{core::ErrorCode::NotFound,
                              "avatar motion telemetry file is not available"};
    }
    std::vector<AvatarMotionSample> samples;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        Json document;
        try {
            document = Json::parse(line);
        } catch (const Json::exception&) {
            return parseError("avatar motion telemetry line is malformed");
        }
        auto parsed = parseSample(document);
        if (!parsed.hasValue()) return parsed.error();
        if (!samples.empty()) {
            if (parsed.value().provider != samples.front().provider) {
                return core::AppError{core::ErrorCode::InvalidArgument,
                                      "avatar telemetry provider changes during playback"};
            }
            if (parsed.value().timestamp <= samples.back().timestamp) {
                return core::AppError{core::ErrorCode::InvalidArgument,
                                      "avatar telemetry timestamps are not strictly increasing"};
            }
        }
        samples.push_back(std::move(parsed).value());
    }
    if (samples.empty()) {
        return parseError("avatar motion telemetry contains no samples");
    }
    return AvatarMotionPlayback{std::move(samples)};
}

core::Result<AvatarMotionSample> AvatarMotionPlayback::sampleAt(
    core::TimestampNs timestamp) const {
    if (samples_.empty()) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "avatar motion playback has no samples"};
    }
    if (timestamp < core::TimestampNs{}) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "avatar playback timestamp is negative"};
    }
    if (timestamp <= samples_.front().timestamp) return samples_.front();
    if (timestamp >= samples_.back().timestamp) return samples_.back();
    const auto next = std::upper_bound(
        samples_.begin(), samples_.end(), timestamp,
        [](core::TimestampNs value, const AvatarMotionSample& sample) {
            return value < sample.timestamp;
        });
    const auto& right = *next;
    const auto& left = *(next - 1);
    const auto total = (right.timestamp - left.timestamp).count();
    const auto elapsed = (timestamp - left.timestamp).count();
    const float ratio = static_cast<float>(elapsed) / static_cast<float>(total);
    return AvatarMotionSample{timestamp, interpolate(left.parameters, right.parameters,
                                                      ratio), left.provider};
}

}  // namespace creator::avatar
