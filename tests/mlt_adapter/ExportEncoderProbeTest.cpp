#include "mlt_adapter/ExportEncoderProbe.h"

#include "ffmpeg_adapter/FfmpegCapabilityProbe.h"

#include <gtest/gtest.h>

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using creator::core::AppError;
using creator::core::ErrorCode;
using creator::core::Result;
using creator::edit_engine::RenderPreset;
using creator::mlt_adapter::ExportEncoderCandidate;
using creator::mlt_adapter::ExportEncoderProbe;

TEST(ExportEncoderProbeTest, SelectsFirstCandidateThatActuallyRenders) {
    const auto preset = RenderPreset::h2641080p30();
    ASSERT_TRUE(preset.hasValue());
    std::vector<std::string> attempted;

    const auto selected = ExportEncoderProbe::select(
        preset.value(), [&](const ExportEncoderCandidate& candidate,
                            const RenderPreset&) -> Result<void> {
            attempted.push_back(candidate.id);
            if (candidate.id == "h264_mf_hw") return creator::core::ok();
            return AppError{ErrorCode::InvalidState, "physical encode failed"};
        });

    ASSERT_TRUE(selected.hasValue()) << selected.error().message();
    EXPECT_EQ(selected.value().selected.id, "h264_mf_hw");
    EXPECT_TRUE(selected.value().selected.hardware);
    EXPECT_EQ(attempted, (std::vector<std::string>{
                             "h264_nvenc", "h264_qsv", "h264_mf_hw"}));
    ASSERT_EQ(selected.value().attempts.size(), 3U);
    EXPECT_FALSE(selected.value().attempts[0].succeeded);
    EXPECT_EQ(selected.value().attempts[0].diagnostic,
              "physical encode failed");
    EXPECT_TRUE(selected.value().attempts[2].succeeded);
}

TEST(ExportEncoderProbeTest, UsesForcedSoftwareFallbackAfterHardwareFailures) {
    const auto preset = RenderPreset::h2642160p30();
    ASSERT_TRUE(preset.hasValue());

    const auto selected = ExportEncoderProbe::select(
        preset.value(), [](const ExportEncoderCandidate& candidate,
                           const RenderPreset&) -> Result<void> {
            if (candidate.id == "h264_mf_sw") return creator::core::ok();
            return AppError{ErrorCode::InvalidState, "unavailable"};
        });

    ASSERT_TRUE(selected.hasValue()) << selected.error().message();
    EXPECT_EQ(selected.value().selected.id, "h264_mf_sw");
    EXPECT_FALSE(selected.value().selected.hardware);
    EXPECT_FALSE(selected.value().selected.forceMediaFoundationHardware);
    EXPECT_EQ(selected.value().attempts.size(), 4U);
}

TEST(ExportEncoderProbeTest, FailsWhenNoCandidateCompletesPhysicalPreflight) {
    const auto preset = RenderPreset::h2641080p30();
    ASSERT_TRUE(preset.hasValue());

    const auto selected = ExportEncoderProbe::select(
        preset.value(), [](const ExportEncoderCandidate&,
                           const RenderPreset&) -> Result<void> {
            return AppError{ErrorCode::InvalidState, "encode rejected"};
        });

    ASSERT_FALSE(selected.hasValue());
    EXPECT_EQ(selected.error().code(), ErrorCode::InvalidState);
    EXPECT_NE(selected.error().message().find("h264_mf_sw"),
              std::string::npos);
    EXPECT_NE(selected.error().message().find("encode rejected"),
              std::string::npos);
}

class PhysicalExportEncoderProbeTest
    : public ::testing::TestWithParam<Result<RenderPreset> (*)()> {};

TEST_P(PhysicalExportEncoderProbeTest, RendersRequestedProductPreset) {
    const auto preset = GetParam()();
    ASSERT_TRUE(preset.hasValue());
    const auto scratch = std::filesystem::temp_directory_path() /
                         ("creator-studio-encoder-preflight-" +
                          std::to_string(std::chrono::steady_clock::now()
                                             .time_since_epoch()
                                             .count()));
    const auto selected = ExportEncoderProbe::probe(
        std::filesystem::path{CS_TEST_MLT_ROOT}, scratch, preset.value());
    std::error_code ignored;
    std::filesystem::remove_all(scratch, ignored);

    ASSERT_TRUE(selected.hasValue()) << selected.error().message();
    const auto capabilities =
        creator::ffmpeg_adapter::probeFfmpegCapabilities();
    ASSERT_TRUE(capabilities.hasValue());
    EXPECT_TRUE(std::any_of(
        capabilities.value().encoders.begin(),
        capabilities.value().encoders.end(), [&](const auto& capability) {
            return capability.name == selected.value().selected.videoCodec &&
                   capability.available;
        }));
    EXPECT_FALSE(selected.value().attempts.empty());
    EXPECT_TRUE(selected.value().attempts.back().succeeded);
    EXPECT_EQ(selected.value().attempts.back().candidate,
              selected.value().selected);
}

INSTANTIATE_TEST_SUITE_P(
    ProductPresets, PhysicalExportEncoderProbeTest,
    ::testing::Values(&RenderPreset::h2641080p30,
                      &RenderPreset::h2642160p30));

}  // namespace
