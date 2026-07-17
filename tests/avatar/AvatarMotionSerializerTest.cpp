#include "avatar/AvatarMotionSerializer.h"

#include "avatar/AvatarMotionSample.h"
#include "avatar/AvatarProviderId.h"
#include "avatar/ExpressionParameters.h"
#include "core/Timebase.h"

#include <gtest/gtest.h>
#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace {

using creator::avatar::AvatarMotionSample;
using creator::avatar::AvatarMotionSerializer;
using creator::avatar::AvatarProviderId;
using creator::avatar::ExpressionParameters;
using creator::core::DurationNs;
using creator::core::TimestampNs;

// Collects every schema violation rather than throwing on the first one, the
// same shape project_store::ManifestSchemaValidator uses against
// project.schema.json - mirrored here against event.schema.json, the schema
// this task's whole payoff depends on.
class EventSchemaCollectingErrorHandler final : public nlohmann::json_schema::basic_error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& instance,
               const std::string& message) override {
        basic_error_handler::error(pointer, instance, message);
        failed_ = true;
    }

    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    bool failed_{false};
};

const nlohmann::json_schema::json_validator& eventSchemaValidator() {
    static const nlohmann::json_schema::json_validator validator = [] {
        std::ifstream schemaFile(CS_EVENT_SCHEMA_PATH);
        nlohmann::json schemaJson;
        schemaFile >> schemaJson;

        nlohmann::json_schema::json_validator compiled{
            nullptr, nlohmann::json_schema::default_string_format_check};
        compiled.set_root_schema(schemaJson);
        return compiled;
    }();
    return validator;
}

[[nodiscard]] bool validatesAgainstEventSchema(const nlohmann::json& document) {
    EventSchemaCollectingErrorHandler errors;
    eventSchemaValidator().validate(document, errors);
    return !errors.failed();
}

AvatarMotionSample makeSample(std::int64_t timestampNs, std::string providerId,
                               ExpressionParameters parameters) {
    return AvatarMotionSample{
        .timestamp = TimestampNs{DurationNs{timestampNs}},
        .parameters = parameters,
        .provider = AvatarProviderId::create(std::move(providerId)).value(),
    };
}

ExpressionParameters sampleParameters() {
    ExpressionParameters parameters{};
    parameters.eyeOpenLeft = 0.25F;
    parameters.eyeOpenRight = 0.5F;
    parameters.browUpLeft = 0.1F;
    parameters.browUpRight = 0.2F;
    parameters.mouthOpen = 0.75F;
    parameters.mouthWide = 0.6F;
    parameters.headYaw = -0.3F;
    parameters.headPitch = 0.4F;
    parameters.headRoll = -0.1F;
    return parameters;
}

TEST(AvatarMotionSerializerTest, ToJsonProducesExactTopLevelFieldSet) {
    const AvatarMotionSample sample =
        makeSample(1'000'000, "fake-provider", sampleParameters());
    const nlohmann::json json = AvatarMotionSerializer{}.toJson(sample);

    std::set<std::string> keys;
    for (const auto& [key, value] : json.items()) {
        keys.insert(key);
    }
    EXPECT_EQ(keys, (std::set<std::string>{"tNs", "type", "provider", "parameters"}));

    EXPECT_EQ(json["tNs"].get<std::int64_t>(), 1'000'000);
    EXPECT_EQ(json["type"].get<std::string>(), "avatar.motion");
    EXPECT_EQ(json["provider"].get<std::string>(), "fake-provider");
}

TEST(AvatarMotionSerializerTest, ParametersHasExactlyNineFieldsAllNumbers) {
    const AvatarMotionSample sample =
        makeSample(0, "fake-provider", sampleParameters());
    const nlohmann::json json = AvatarMotionSerializer{}.toJson(sample);
    const nlohmann::json& parameters = json["parameters"];

    ASSERT_TRUE(parameters.is_object());
    EXPECT_EQ(parameters.size(), 9U);

    const std::set<std::string> expectedFields{
        "eyeOpenLeft", "eyeOpenRight", "browUpLeft", "browUpRight", "mouthOpen",
        "mouthWide",   "headYaw",      "headPitch",  "headRoll",
    };
    std::set<std::string> actualFields;
    for (const auto& [key, value] : parameters.items()) {
        actualFields.insert(key);
        EXPECT_TRUE(value.is_number()) << key << " must serialize as a JSON number";
    }
    EXPECT_EQ(actualFields, expectedFields);

    EXPECT_FLOAT_EQ(parameters["mouthOpen"].get<float>(), 0.75F);
    EXPECT_FLOAT_EQ(parameters["headYaw"].get<float>(), -0.3F);
}

TEST(AvatarMotionSerializerTest, SerializedAvatarMotionValidatesAgainstEventSchema) {
    const AvatarMotionSample sample =
        makeSample(42, "fake-provider", sampleParameters());
    const nlohmann::json json = AvatarMotionSerializer{}.toJson(sample);

    EXPECT_TRUE(validatesAgainstEventSchema(json));
}

// Contract documented on AvatarMotionSerializer::toJson: a negative timestamp
// is NOT clamped to 0. It is serialized as-is, and the schema's `tNs`
// `minimum: 0` then rejects the whole document - the error surfaces at the
// validation boundary instead of being silently patched inside the
// serializer (CLAUDE.md 9).
TEST(AvatarMotionSerializerTest, NegativeTimestampFailsEventSchemaValidation) {
    const AvatarMotionSample sample =
        makeSample(-1, "fake-provider", sampleParameters());
    const nlohmann::json json = AvatarMotionSerializer{}.toJson(sample);

    EXPECT_EQ(json["tNs"].get<std::int64_t>(), -1)
        << "the serializer must not silently clamp a negative timestamp";
    EXPECT_FALSE(validatesAgainstEventSchema(json));
}

TEST(AvatarMotionSerializerTest, ToNdjsonLineHasExactlyOneTrailingNewlineAndNoneEmbedded) {
    const AvatarMotionSample sample =
        makeSample(7, "fake-provider", sampleParameters());
    const std::string line = AvatarMotionSerializer{}.toNdjsonLine(sample);

    ASSERT_FALSE(line.empty());
    EXPECT_EQ(line.back(), '\n');
    EXPECT_EQ(std::count(line.begin(), line.end(), '\n'), 1);

    const std::string withoutTrailingNewline = line.substr(0, line.size() - 1);
    EXPECT_EQ(withoutTrailingNewline.find('\n'), std::string::npos);

    const nlohmann::json parsedBack = nlohmann::json::parse(withoutTrailingNewline);
    EXPECT_EQ(parsedBack, AvatarMotionSerializer{}.toJson(sample));
}

TEST(AvatarMotionSerializerTest, DistinctSamplesProduceDistinctJson) {
    const AvatarMotionSerializer serializer;

    const AvatarMotionSample first = makeSample(100, "provider-a", sampleParameters());

    ExpressionParameters differentParameters = sampleParameters();
    differentParameters.mouthOpen = 0.0F;
    differentParameters.headYaw = 0.9F;
    const AvatarMotionSample second =
        makeSample(200, "provider-b", differentParameters);

    const nlohmann::json firstJson = serializer.toJson(first);
    const nlohmann::json secondJson = serializer.toJson(second);

    EXPECT_NE(firstJson, secondJson);
    EXPECT_NE(firstJson["tNs"], secondJson["tNs"]);
    EXPECT_NE(firstJson["provider"], secondJson["provider"]);
    EXPECT_NE(firstJson["parameters"]["mouthOpen"], secondJson["parameters"]["mouthOpen"]);
    EXPECT_NE(firstJson["parameters"]["headYaw"], secondJson["parameters"]["headYaw"]);

    EXPECT_TRUE(validatesAgainstEventSchema(firstJson));
    EXPECT_TRUE(validatesAgainstEventSchema(secondJson));
}

}  // namespace
