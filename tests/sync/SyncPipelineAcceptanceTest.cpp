#include "sync/AudioRateCompensator.h"
#include "sync/ClockCoordinator.h"
#include "sync/VideoSyncPlanner.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>

namespace {

using creator::core::TimestampNs;
using creator::domain::SourceId;
using creator::media::PixelFormat;
using creator::media::VideoFrame;
using creator::synchronization::AudioRateCompensator;
using creator::synchronization::ClockCoordinator;
using creator::synchronization::ClockSourceConfig;
using creator::synchronization::SyncMediaKind;
using creator::synchronization::VideoSyncPlanner;

TimestampNs at(std::chrono::nanoseconds value) { return TimestampNs{value}; }

ClockSourceConfig source(const char* id, SyncMediaKind kind,
                         std::uint32_t priority) {
    return {.sourceId = SourceId::create(id).value(),
            .mediaKind = kind,
            .masterPriority = priority};
}

std::chrono::nanoseconds oscillatorTimestamp(std::chrono::nanoseconds elapsed,
                                             double ppm) {
    const auto drift = static_cast<std::int64_t>(
        std::llround(static_cast<double>(elapsed.count()) * ppm / 1'000'000.0));
    return elapsed + std::chrono::nanoseconds{drift};
}

VideoFrame frame() {
    return VideoFrame{.width = 2,
                      .height = 2,
                      .visibleRect = {0, 0, 2, 2},
                      .contentWidth = 2,
                      .contentHeight = 2,
                      .pixelFormat = PixelFormat::Bgra8,
                      .platformHandle = std::make_shared<int>(7)};
}

TEST(SyncPipelineAcceptanceTest,
     AcceleratedTwoHoursBoundsOppositeDriftJitterAndVideoOutput) {
    const auto microphone = SourceId::create("microphone").value();
    const auto secondaryAudio = SourceId::create("system-audio").value();
    const auto camera = SourceId::create("camera").value();
    auto coordinator = ClockCoordinator::create(
        {source("camera", SyncMediaKind::Video, 2),
         source("system-audio", SyncMediaKind::Audio, 1),
         source("microphone", SyncMediaKind::Audio, 0)})
                           .value();
    auto video = VideoSyncPlanner::create(std::chrono::nanoseconds{33'333'333}, 2)
                     .value();
    AudioRateCompensator audio;
    const std::array jitter{std::chrono::milliseconds{-7},
                            std::chrono::milliseconds{5},
                            std::chrono::milliseconds{-3},
                            std::chrono::milliseconds{8}};

    ASSERT_TRUE(coordinator->observe(microphone, at({}), at({})).hasValue());
    ASSERT_TRUE(coordinator->observe(secondaryAudio, at({}), at({})).hasValue());
    ASSERT_TRUE(coordinator->observe(camera, at({}), at({})).hasValue());

    TimestampNs lastVideoTimestamp{};
    bool hasVideoOutput = false;
    creator::synchronization::ClockCorrection finalAudio;
    creator::synchronization::ClockCorrection finalVideo;
    std::int64_t totalAudioCompensation = 0;
    for (int second = 10; second <= 2 * 60 * 60; second += 10) {
        const auto masterTime = std::chrono::seconds{second};
        const auto callbackTime = masterTime + jitter[static_cast<std::size_t>(second / 10) % jitter.size()];
        ASSERT_TRUE(coordinator->observe(microphone, at(masterTime), at(masterTime))
                        .hasValue());

        auto audioCorrection = coordinator->observe(
            secondaryAudio, at(oscillatorTimestamp(callbackTime, 500.0)),
            at(callbackTime));
        ASSERT_TRUE(audioCorrection.hasValue());
        finalAudio = audioCorrection.value();
        ASSERT_GE(finalAudio.audioRateRatio, 0.999);
        ASSERT_LE(finalAudio.audioRateRatio, 1.001);
        auto samples = audio.next(480'000, finalAudio.audioRateRatio);
        ASSERT_TRUE(samples.hasValue());
        totalAudioCompensation += samples.value();

        auto videoCorrection = coordinator->observe(
            camera, at(oscillatorTimestamp(callbackTime, -400.0)),
            at(callbackTime));
        ASSERT_TRUE(videoCorrection.hasValue());
        finalVideo = videoCorrection.value();
        auto planned = video->plan(frame(), finalVideo.correctedTimestamp);
        ASSERT_TRUE(planned.hasValue());
        for (const auto& output : planned.value().frames) {
            if (hasVideoOutput) EXPECT_LT(lastVideoTimestamp, output.timestamp);
            lastVideoTimestamp = output.timestamp;
            hasVideoOutput = true;
        }
    }

    const auto finalMasterTime = std::chrono::hours{2} + jitter[0];
    EXPECT_LT(std::abs((finalAudio.correctedTimestamp - at(finalMasterTime)).count()),
              std::chrono::milliseconds{40}.count() * 1'000'000LL);
    EXPECT_LT(std::abs((finalVideo.correctedTimestamp - at(finalMasterTime)).count()),
              std::chrono::milliseconds{40}.count() * 1'000'000LL);
    EXPECT_NEAR(finalAudio.audioRateRatio, 0.9995, 0.00005);
    EXPECT_GT(std::abs(totalAudioCompensation), 0);
    EXPECT_TRUE(hasVideoOutput);
    EXPECT_EQ(coordinator->snapshot().sources.size(), 3u);
    EXPECT_LE(video->snapshot().framesDuplicated, 2u * 720u);
}

}  // namespace
