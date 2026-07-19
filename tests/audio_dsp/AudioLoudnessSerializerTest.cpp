#include "audio_dsp/AudioLoudnessSerializer.h"

#include "audio_dsp/AudioLoudnessSample.h"
#include "audio_dsp/LoudnessMeter.h"
#include "core/AppError.h"
#include "core/Timebase.h"
#include "support/EventSchemaValidator.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>

namespace {

using creator::audio_dsp::AudioLoudnessSample;
using creator::audio_dsp::AudioLoudnessSerializer;
using creator::audio_dsp::LoudnessMeter;
using creator::audio_dsp::test::validatesAgainstEventSchema;
using creator::core::ErrorCode;
using creator::core::TimestampNs;

TimestampNs atNs(std::int64_t ns) {
    return TimestampNs{std::chrono::nanoseconds{ns}};
}

TEST(AudioLoudnessSerializerTest, FullyMeasuredSampleValidatesAgainstEventSchema) {
    AudioLoudnessSample sample;
    sample.tNs = atNs(1'500'000'000);
    sample.integratedLufs = -23.0;
    sample.momentaryLufs = -21.4;
    sample.shortTermLufs = -22.1;
    sample.truePeakDbtp = -1.2;
    sample.sourceId = "mic-1";

    const auto json = AudioLoudnessSerializer::toJson(sample);
    ASSERT_TRUE(json.hasValue());

    std::string whyNot;
    EXPECT_TRUE(validatesAgainstEventSchema(json.value(), &whyNot)) << whyNot;
    EXPECT_EQ(json.value().at("type"), "audio.loudness");
    EXPECT_EQ(json.value().at("tNs"), 1'500'000'000);
    EXPECT_EQ(json.value().at("integratedLufs"), -23.0);
    EXPECT_EQ(json.value().at("sourceId"), "mic-1");
}

TEST(AudioLoudnessSerializerTest, NoMeasurementMapsToJsonNullAndStillValidates) {
    AudioLoudnessSample sample;
    sample.tNs = atNs(0);
    sample.integratedLufs = LoudnessMeter::kNoMeasurement;  // -inf
    sample.momentaryLufs = LoudnessMeter::kNoMeasurement;   // -inf
    sample.shortTermLufs = -30.0;
    sample.truePeakDbtp = -6.0;

    const auto json = AudioLoudnessSerializer::toJson(sample);
    ASSERT_TRUE(json.hasValue());

    std::string whyNot;
    EXPECT_TRUE(validatesAgainstEventSchema(json.value(), &whyNot)) << whyNot;
    EXPECT_TRUE(json.value().at("integratedLufs").is_null());
    EXPECT_TRUE(json.value().at("momentaryLufs").is_null());
    EXPECT_TRUE(json.value().at("shortTermLufs").is_number());
    EXPECT_FALSE(json.value().contains("sourceId"));
}

TEST(AudioLoudnessSerializerTest, NanTruePeakMapsToNull) {
    AudioLoudnessSample sample;
    sample.tNs = atNs(42);
    sample.truePeakDbtp = std::numeric_limits<double>::quiet_NaN();

    const auto json = AudioLoudnessSerializer::toJson(sample);
    ASSERT_TRUE(json.hasValue());

    std::string whyNot;
    EXPECT_TRUE(validatesAgainstEventSchema(json.value(), &whyNot)) << whyNot;
    EXPECT_TRUE(json.value().at("truePeakDbtp").is_null());
}

TEST(AudioLoudnessSerializerTest, NegativeTimestampIsRejected) {
    AudioLoudnessSample sample;
    sample.tNs = atNs(-1);
    sample.truePeakDbtp = -1.0;

    const auto json = AudioLoudnessSerializer::toJson(sample);
    ASSERT_FALSE(json.hasValue());
    EXPECT_EQ(json.error().code(), ErrorCode::InvalidArgument);
}

}  // namespace
