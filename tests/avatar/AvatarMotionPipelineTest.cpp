#include "avatar/AvatarMotionPipeline.h"
#include "fakes/FakeTrackingProvider.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <chrono>
#include <span>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using creator::avatar::AvatarMotionNdjsonSink;
using creator::avatar::AvatarMotionPipeline;
using creator::avatar::CalibrationProfile;
using creator::avatar::ExpressionParameters;
using creator::avatar::TrackingResult;
using creator::core::DurationNs;
using creator::core::TimestampNs;

class AvatarMotionPipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() / "creator-studio-avatar-pipeline";
        std::error_code error;
        fs::remove_all(root_, error);
        fs::create_directories(root_ / "telemetry");
    }
    void TearDown() override {
        std::error_code error;
        fs::remove_all(root_, error);
    }
    fs::path root_;
};

TrackingResult face(TimestampNs timestamp, float confidence,
                    float mouthOpen) {
    TrackingResult result{};
    result.timestamp = timestamp;
    result.faceFound = true;
    result.confidence = confidence;
    result.raw = ExpressionParameters::neutral();
    result.raw.mouthOpen = mouthOpen;
    return result;
}

TEST_F(AvatarMotionPipelineTest, SelectsNormalizesSmoothsAndPersistsPrimaryFace) {
    AvatarMotionNdjsonSink sink(root_ / "telemetry");
    AvatarMotionPipeline pipeline{
        creator::avatar::AvatarProviderId::create("openseeface").value(),
        CalibrationProfile::identity(), sink};
    const std::vector<TrackingResult> candidates{
        face(TimestampNs{}, 0.2F, 0.1F),
        face(TimestampNs{}, 0.9F, 0.7F)};

    const auto sample = pipeline.process(candidates);

    ASSERT_TRUE(sample.hasValue()) << sample.error().message();
    EXPECT_FLOAT_EQ(sample.value().parameters.mouthOpen, 0.7F);
    EXPECT_EQ(sample.value().provider.value(), "openseeface");
    const auto path = root_ / "telemetry" / AvatarMotionNdjsonSink::kFileName;
    EXPECT_TRUE(fs::is_regular_file(path));
    std::ifstream input(path);
    std::string line;
    ASSERT_TRUE(std::getline(input, line));
    EXPECT_NE(line.find("openseeface"), std::string::npos);
}

TEST_F(AvatarMotionPipelineTest, LostFaceEmitsNeutralAndEmptyBatchIsRejected) {
    AvatarMotionNdjsonSink sink(root_ / "telemetry");
    AvatarMotionPipeline pipeline{
        creator::avatar::AvatarProviderId::create("openseeface").value(),
        CalibrationProfile::identity(), sink};
    TrackingResult lost{};
    lost.timestamp = TimestampNs{DurationNs{1s}};
    lost.faceFound = false;
    lost.confidence = 0.0F;
    const std::vector<TrackingResult> candidates{lost};

    const auto sample = pipeline.process(candidates);
    ASSERT_TRUE(sample.hasValue()) << sample.error().message();
    EXPECT_EQ(sample.value().parameters, ExpressionParameters::neutral());
    const auto empty = pipeline.process(std::span<const TrackingResult>{});
    ASSERT_FALSE(empty.hasValue());
    EXPECT_EQ(empty.error().code(), creator::core::ErrorCode::InvalidArgument);
}

TEST_F(AvatarMotionPipelineTest, ResetMakesNextSampleColdStartAgain) {
    AvatarMotionNdjsonSink sink(root_ / "telemetry");
    AvatarMotionPipeline pipeline{
        creator::avatar::AvatarProviderId::create("fake").value(),
        CalibrationProfile::identity(), sink};
    const auto first = face(TimestampNs{}, 1.0F, 0.0F);
    const auto second = face(TimestampNs{DurationNs{33'000'000}}, 1.0F, 1.0F);
    ASSERT_TRUE(pipeline.process(std::span<const TrackingResult>{&first, 1}).hasValue());
    const auto beforeReset =
        pipeline.process(std::span<const TrackingResult>{&second, 1});
    ASSERT_TRUE(beforeReset.hasValue()) << beforeReset.error().message();
    pipeline.reset();
    const auto afterReset = pipeline.process(
        std::span<const TrackingResult>{&second, 1});
    ASSERT_TRUE(afterReset.hasValue()) << afterReset.error().message();
    EXPECT_FLOAT_EQ(afterReset.value().parameters.mouthOpen, 1.0F);
}

TEST_F(AvatarMotionPipelineTest, ProcessesInProcessProviderFrameThroughSamePipeline) {
    AvatarMotionNdjsonSink sink(root_ / "telemetry");
    AvatarMotionPipeline pipeline{
        creator::avatar::AvatarProviderId::create("fake").value(),
        CalibrationProfile::identity(), sink};
    creator::fakes::FakeTrackingProvider provider{
        {creator::fakes::FakeTrackingProvider::ScriptedFrame{
            .parameters = ExpressionParameters{.mouthOpen = 0.4F}}},
        creator::avatar::AvatarProviderId::create("fake").value()};
    creator::media::VideoFrame frame{};
    frame.timestamp = TimestampNs{DurationNs{42'000'000}};

    const auto sample = pipeline.processFrame(provider, frame);

    ASSERT_TRUE(sample.hasValue()) << sample.error().message();
    EXPECT_FLOAT_EQ(sample.value().parameters.mouthOpen, 0.4F);
    EXPECT_EQ(sample.value().timestamp, frame.timestamp);
}

}  // namespace
