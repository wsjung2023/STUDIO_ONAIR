#include "domain/MediaAsset.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace {

using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::FrameRate;
using creator::domain::AssetAvailability;
using creator::domain::AssetId;
using creator::domain::AudioAssetMetadata;
using creator::domain::MediaAsset;
using creator::domain::MediaKind;
using creator::domain::VideoAssetMetadata;

AssetId assetId(std::string value = "asset-screen") {
    return AssetId::create(std::move(value)).value();
}

VideoAssetMetadata videoMetadata() {
    return VideoAssetMetadata{.width = 1920,
                              .height = 1080,
                              .frameRate = FrameRate::create(60, 1).value()};
}

TEST(MediaAssetTest, CreatesPackageRelativeVideoAsset) {
    const auto created = MediaAsset::create(
        assetId(), MediaKind::Video, "media/session/screen-0001.mkv",
        DurationNs{10'000}, videoMetadata(), std::nullopt, 4'096,
        "sha256:screen", AssetAvailability::Available);

    ASSERT_TRUE(created.hasValue()) << created.error().message();
    EXPECT_EQ(created.value().id(), assetId());
    EXPECT_EQ(created.value().relativePath(), "media/session/screen-0001.mkv");
    EXPECT_EQ(created.value().duration(), DurationNs{10'000});
    ASSERT_TRUE(created.value().video().has_value());
    EXPECT_EQ(created.value().video()->width, 1920);
    EXPECT_FALSE(created.value().audio().has_value());
    EXPECT_EQ(created.value().fileSize(), 4'096U);
}

TEST(MediaAssetTest, CreatesAudioAssetWithLayout) {
    const auto created = MediaAsset::create(
        assetId("asset-mic"), MediaKind::Audio, "audio/session/mic-0001.mka",
        DurationNs{20'000}, std::nullopt,
        AudioAssetMetadata{.sampleRate = 48'000, .channels = 2}, 8'192,
        "sha256:mic", AssetAvailability::Offline);

    ASSERT_TRUE(created.hasValue());
    ASSERT_TRUE(created.value().audio().has_value());
    EXPECT_EQ(created.value().audio()->sampleRate, 48'000);
    EXPECT_EQ(created.value().audio()->channels, 2);
    EXPECT_EQ(created.value().availability(), AssetAvailability::Offline);
}

TEST(MediaAssetTest, RejectsUnsafePackagePaths) {
    for (const std::string path : {"", "/media/a.mkv", "C:/media/a.mkv",
                                   "media/../outside.mkv", "media\\..\\outside.mkv"}) {
        const auto created = MediaAsset::create(
            assetId(), MediaKind::Video, path, DurationNs{1}, videoMetadata(),
            std::nullopt, 1, "hash", AssetAvailability::Available);
        ASSERT_FALSE(created.hasValue()) << path;
        EXPECT_EQ(created.error().code(), ErrorCode::InvalidArgument);
    }
}

TEST(MediaAssetTest, RejectsInvalidDurationAndMetadata) {
    const auto zeroDuration = MediaAsset::create(
        assetId(), MediaKind::Video, "media/a.mkv", DurationNs::zero(),
        videoMetadata(), std::nullopt, 1, "hash", AssetAvailability::Available);
    const auto missingVideo = MediaAsset::create(
        assetId(), MediaKind::Video, "media/a.mkv", DurationNs{1},
        std::nullopt, std::nullopt, 1, "hash", AssetAvailability::Available);
    const auto invalidAudio = MediaAsset::create(
        assetId(), MediaKind::Audio, "audio/a.mka", DurationNs{1},
        std::nullopt, AudioAssetMetadata{.sampleRate = 0, .channels = 2},
        1, "hash", AssetAvailability::Available);

    EXPECT_FALSE(zeroDuration.hasValue());
    EXPECT_FALSE(missingVideo.hasValue());
    EXPECT_FALSE(invalidAudio.hasValue());
}

}  // namespace
