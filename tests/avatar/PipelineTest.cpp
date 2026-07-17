// End-to-end proof of Stage A's avatar tracking pipeline (R3 plan, Task A5):
//
//   FakeCaptureSource --frame--> FakeTrackingProvider.process
//     --TrackingResult--> ExpressionNormalizer(CalibrationProfile)
//     --ExpressionParameters--> AvatarMotionSample
//     --> AvatarMotionNdjsonSink.append --> avatar-motion.ndjson
//
// Every stage is deterministic (no clock, no sleep, no thread, no pixels), so
// this test pins exact parameter values and an exact line count, then
// validates every emitted line against schemas/event.schema.json - the same
// authority AvatarMotionSerializerTest and AvatarMotionNdjsonSinkTest use.

#include "avatar/AvatarMotionNdjsonSink.h"
#include "avatar/AvatarMotionSample.h"
#include "avatar/AvatarProviderId.h"
#include "avatar/CalibrationProfile.h"
#include "avatar/ExpressionNormalizer.h"
#include "avatar/ExpressionParameters.h"
#include "avatar/TrackingResult.h"
#include "capture/ICaptureSource.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "fakes/FakeCaptureSource.h"
#include "fakes/FakeTrackingProvider.h"
#include "media/MediaTypes.h"

#include <gtest/gtest.h>
#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

using creator::avatar::AvatarMotionNdjsonSink;
using creator::avatar::AvatarMotionSample;
using creator::avatar::AvatarProviderId;
using creator::avatar::CalibrationProfile;
using creator::avatar::ExpressionNormalizer;
using creator::avatar::ExpressionParameters;
using creator::capture::CaptureConfig;
using creator::domain::SourceId;
using creator::fakes::FakeCaptureSource;
using creator::fakes::FakeTrackingProvider;

// Same collecting-error-handler shape used by AvatarMotionSerializerTest and
// AvatarMotionNdjsonSinkTest against the same schema.
class EventSchemaCollectingErrorHandler final : public nlohmann::json_schema::basic_error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer, const nlohmann::json& instance,
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

std::vector<std::string> readLines(const fs::path& file) {
    std::ifstream in{file, std::ios::binary};
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}

class PipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        telemetryDir_ = fs::temp_directory_path() /
                       ("cs_test_" + std::string{info->test_suite_name()} + "_" +
                        std::string{info->name()});
        std::error_code ec;
        fs::remove_all(telemetryDir_, ec);
        fs::create_directories(telemetryDir_, ec);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(telemetryDir_, ec);
    }

    [[nodiscard]] fs::path ndjsonFile() const {
        return telemetryDir_ / AvatarMotionNdjsonSink::kFileName;
    }

    fs::path telemetryDir_;
};

constexpr float kTolerance = 1e-4F;

// The performer's own captured neutral face: a resting left eye that reads
// 0.1 raw rather than the documented 0.0. Every scripted frame below is
// designed around this baseline, so the expected output only matches if
// calibration was actually applied (not merely passed through) end to end.
ExpressionParameters neutralBaseline() {
    ExpressionParameters baseline{};
    baseline.eyeOpenLeft = 0.1F;
    return baseline;
}

std::vector<FakeTrackingProvider::ScriptedFrame> fourFrameScript() {
    ExpressionParameters frame0{};
    frame0.mouthOpen = 0.5F;
    frame0.eyeOpenLeft = 0.3F;  // calibrated: 0.3 - 0.1 baseline = 0.2

    ExpressionParameters frame1{};
    frame1.headYaw = 0.4F;
    frame1.headPitch = -0.2F;

    // Frame 2 scripts a face-tracking loss. Its raw expression is a large,
    // clearly-nonzero mouthOpen that must NOT reach the telemetry: the
    // normalizer's faceFound==false path replaces it with neutral() before it
    // ever reaches AvatarMotionSample. This is the "!faceFound flows
    // end-to-end into telemetry" assertion the task requires.
    ExpressionParameters frame2Raw{};
    frame2Raw.mouthOpen = 0.9F;

    ExpressionParameters frame3{};
    frame3.mouthWide = 0.7F;
    frame3.eyeOpenRight = 0.6F;  // no baseline on this field: stays 0.6

    return {
        FakeTrackingProvider::ScriptedFrame{
            .parameters = frame0, .confidence = 0.9F, .faceFound = true},
        FakeTrackingProvider::ScriptedFrame{
            .parameters = frame1, .confidence = 0.85F, .faceFound = true},
        FakeTrackingProvider::ScriptedFrame{
            .parameters = frame2Raw, .confidence = 0.0F, .faceFound = false},
        FakeTrackingProvider::ScriptedFrame{
            .parameters = frame3, .confidence = 0.7F, .faceFound = true},
    };
}

TEST_F(PipelineTest, FullChainProducesExactSchemaValidTelemetry) {
    constexpr int kFrameCount = 4;

    FakeCaptureSource captureSource{SourceId::create("screen-1").value(), "Fake Screen"};
    ASSERT_TRUE(captureSource.start(CaptureConfig{}).hasValue());

    const AvatarProviderId providerId = AvatarProviderId::create("fake-tracker").value();
    FakeTrackingProvider provider{fourFrameScript(), providerId};

    const auto calibration = CalibrationProfile::fromNeutral(neutralBaseline());
    ASSERT_TRUE(calibration.hasValue()) << calibration.error().message();
    const ExpressionNormalizer normalizer{calibration.value()};

    AvatarMotionNdjsonSink sink{telemetryDir_};

    std::vector<creator::core::TimestampNs> frameTimestamps;
    for (int i = 0; i < kFrameCount; ++i) {
        const auto frame = captureSource.tick();
        ASSERT_TRUE(frame.hasValue()) << "frame " << i;
        frameTimestamps.push_back(frame.value().timestamp);

        const auto tracked = provider.process(frame.value());
        ASSERT_TRUE(tracked.hasValue()) << "frame " << i;
        // Pins the "timestamp comes from the frame" contract at the pipeline
        // level, not just inside FakeTrackingProviderTest.
        EXPECT_EQ(tracked.value().timestamp, frame.value().timestamp) << "frame " << i;

        const ExpressionParameters normalized = normalizer.normalize(tracked.value());

        const AvatarMotionSample sample{
            .timestamp = frame.value().timestamp,
            .parameters = normalized,
            .provider = provider.providerId(),
        };
        const auto appended = sink.append(sample);
        ASSERT_TRUE(appended.hasValue()) << "frame " << i << ": " << appended.error().message();
    }

    const std::vector<std::string> lines = readLines(ndjsonFile());
    ASSERT_EQ(lines.size(), static_cast<std::size_t>(kFrameCount));

    std::vector<nlohmann::json> parsed;
    for (const std::string& line : lines) {
        const nlohmann::json document = nlohmann::json::parse(line);
        EXPECT_TRUE(validatesAgainstEventSchema(document)) << line;
        parsed.push_back(document);
    }

    // Frame 0: identity on mouthOpen, baseline-shifted eyeOpenLeft.
    EXPECT_EQ(parsed[0]["tNs"].get<std::int64_t>(), frameTimestamps[0].time_since_epoch().count());
    EXPECT_EQ(parsed[0]["provider"].get<std::string>(), "fake-tracker");
    EXPECT_NEAR(parsed[0]["parameters"]["mouthOpen"].get<float>(), 0.5F, kTolerance);
    EXPECT_NEAR(parsed[0]["parameters"]["eyeOpenLeft"].get<float>(), 0.2F, kTolerance);

    // Frame 1: head-angle fields pass through unchanged (no baseline shift on
    // these fields).
    EXPECT_EQ(parsed[1]["tNs"].get<std::int64_t>(), frameTimestamps[1].time_since_epoch().count());
    EXPECT_NEAR(parsed[1]["parameters"]["headYaw"].get<float>(), 0.4F, kTolerance);
    EXPECT_NEAR(parsed[1]["parameters"]["headPitch"].get<float>(), -0.2F, kTolerance);

    // Frame 2: faceFound == false end to end -> every parameter is neutral
    // (0), even though the script's raw mouthOpen was 0.9. This is the
    // required "!faceFound flows into telemetry" proof.
    EXPECT_EQ(parsed[2]["tNs"].get<std::int64_t>(), frameTimestamps[2].time_since_epoch().count());
    for (const auto& [key, value] : parsed[2]["parameters"].items()) {
        EXPECT_NEAR(value.get<float>(), 0.0F, kTolerance) << "frame 2 field " << key;
    }

    // Frame 3: fields with no baseline shift pass through unchanged.
    EXPECT_EQ(parsed[3]["tNs"].get<std::int64_t>(), frameTimestamps[3].time_since_epoch().count());
    EXPECT_NEAR(parsed[3]["parameters"]["mouthWide"].get<float>(), 0.7F, kTolerance);
    EXPECT_NEAR(parsed[3]["parameters"]["eyeOpenRight"].get<float>(), 0.6F, kTolerance);

    // Timestamps strictly increase with frame index: FakeCaptureSource's
    // timing contract (proven independently in FakeCaptureSourceTest) really
    // did flow through the whole chain rather than being dropped somewhere.
    for (std::size_t i = 1; i < frameTimestamps.size(); ++i) {
        EXPECT_GT(frameTimestamps[i], frameTimestamps[i - 1]) << "index " << i;
    }
}

TEST_F(PipelineTest, RunningTheSameScriptTwiceProducesIdenticalNdjson) {
    auto runPipelineOnce = [](const fs::path& dir) {
        FakeCaptureSource captureSource{SourceId::create("screen-1").value(), "Fake Screen"};
        ASSERT_TRUE(captureSource.start(CaptureConfig{}).hasValue());
        FakeTrackingProvider provider{fourFrameScript()};
        const ExpressionNormalizer normalizer{CalibrationProfile::identity()};
        AvatarMotionNdjsonSink sink{dir};

        for (int i = 0; i < 4; ++i) {
            const auto frame = captureSource.tick();
            ASSERT_TRUE(frame.hasValue()) << "frame " << i;
            const auto tracked = provider.process(frame.value());
            ASSERT_TRUE(tracked.hasValue()) << "frame " << i;
            const ExpressionParameters normalized = normalizer.normalize(tracked.value());
            const auto appended = sink.append(AvatarMotionSample{
                .timestamp = frame.value().timestamp,
                .parameters = normalized,
                .provider = provider.providerId(),
            });
            ASSERT_TRUE(appended.hasValue()) << "frame " << i;
        }
    };

    const fs::path secondDir = telemetryDir_ / "second-run";
    std::error_code ec;
    fs::create_directories(secondDir, ec);

    runPipelineOnce(telemetryDir_);
    runPipelineOnce(secondDir);

    const std::vector<std::string> firstLines = readLines(ndjsonFile());
    const std::vector<std::string> secondLines =
        readLines(secondDir / AvatarMotionNdjsonSink::kFileName);

    ASSERT_EQ(firstLines.size(), secondLines.size());
    ASSERT_EQ(firstLines.size(), 4U);
    for (std::size_t i = 0; i < firstLines.size(); ++i) {
        EXPECT_EQ(firstLines[i], secondLines[i]) << "line " << i;
    }
}

}  // namespace
