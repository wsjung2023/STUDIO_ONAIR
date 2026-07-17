#include "sync/ClockCoordinator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <limits>
#include <vector>

namespace {

using creator::core::TimestampNs;
using creator::domain::SourceId;
using creator::synchronization::ClockCoordinator;
using creator::synchronization::ClockSourceConfig;
using creator::synchronization::SyncMediaKind;

TimestampNs at(std::chrono::nanoseconds value) { return TimestampNs{value}; }

ClockSourceConfig source(const char* id, SyncMediaKind kind, std::uint32_t priority) {
    return {.sourceId = SourceId::create(id).value(),
            .mediaKind = kind,
            .masterPriority = priority};
}

std::unique_ptr<ClockCoordinator> coordinator() {
    return ClockCoordinator::create(
               {source("screen", SyncMediaKind::Video, 2),
                source("system", SyncMediaKind::Audio, 1),
                source("microphone", SyncMediaKind::Audio, 0)})
        .value();
}

TEST(ClockCoordinatorTest, SelectsLowestPriorityAudioMasterDeterministically) {
    auto clock = coordinator();

    EXPECT_EQ(clock->masterSourceId().value(), "microphone");
    const auto snapshot = clock->snapshot();
    ASSERT_EQ(snapshot.sources.size(), 3u);
    EXPECT_EQ(std::count_if(snapshot.sources.begin(), snapshot.sources.end(),
                            [](const auto& item) { return item.master; }),
              1);
}

TEST(ClockCoordinatorTest, InterpolatesMasterAtFollowerCallbackTime) {
    auto clock = coordinator();
    const auto microphone = SourceId::create("microphone").value();
    const auto screen = SourceId::create("screen").value();
    ASSERT_TRUE(clock->observe(microphone, at(std::chrono::seconds{10}),
                               at(std::chrono::seconds{100}))
                    .hasValue());

    const auto correction = clock->observe(
        screen, at(std::chrono::milliseconds{10'500}),
        at(std::chrono::milliseconds{100'500}));

    ASSERT_TRUE(correction.hasValue());
    EXPECT_TRUE(correction.value().synchronized);
    EXPECT_EQ(correction.value().drift, std::chrono::nanoseconds::zero());
    EXPECT_EQ(correction.value().correctedTimestamp,
              at(std::chrono::milliseconds{10'500}));
}

TEST(ClockCoordinatorTest, RemovesConstantFollowerOffsetWithoutChangingRate) {
    auto clock = coordinator();
    const auto microphone = SourceId::create("microphone").value();
    const auto system = SourceId::create("system").value();
    ASSERT_TRUE(clock->observe(microphone, at(std::chrono::seconds{1}),
                               at(std::chrono::seconds{20}))
                    .hasValue());

    const auto correction = clock->observe(
        system, at(std::chrono::milliseconds{1'025}), at(std::chrono::seconds{20}));

    ASSERT_TRUE(correction.hasValue());
    EXPECT_EQ(correction.value().drift, std::chrono::milliseconds{25});
    EXPECT_EQ(correction.value().correctedTimestamp, at(std::chrono::seconds{1}));
    EXPECT_DOUBLE_EQ(correction.value().audioRateRatio, 1.0);
}

TEST(ClockCoordinatorTest, AcceleratedTwoHoursConvergesFiveHundredPpmBelowFortyMs) {
    auto clock = coordinator();
    const auto microphone = SourceId::create("microphone").value();
    const auto system = SourceId::create("system").value();
    ASSERT_TRUE(clock->observe(microphone, at({}), at({})).hasValue());
    ASSERT_TRUE(clock->observe(system, at({}), at({})).hasValue());

    creator::synchronization::ClockCorrection last;
    for (int second = 10; second <= 2 * 60 * 60; second += 10) {
        const auto master = std::chrono::seconds{second};
        const auto drift = std::chrono::microseconds{second * 500};
        ASSERT_TRUE(clock->observe(microphone, at(master), at(master)).hasValue());
        auto observed = clock->observe(system, at(master + drift), at(master));
        ASSERT_TRUE(observed.hasValue());
        last = observed.value();
    }

    EXPECT_LT(std::abs((last.correctedTimestamp - at(std::chrono::hours{2})).count()),
              std::chrono::milliseconds{40}.count() * 1'000'000LL);
    EXPECT_NEAR(last.audioRateRatio, 0.9995, 0.00005);
}

TEST(ClockCoordinatorTest, ClampsExtremeRateCorrectionToOneThousandPpm) {
    auto clock = coordinator();
    const auto microphone = SourceId::create("microphone").value();
    const auto system = SourceId::create("system").value();
    ASSERT_TRUE(clock->observe(microphone, at({}), at({})).hasValue());
    ASSERT_TRUE(clock->observe(system, at({}), at({})).hasValue());
    creator::synchronization::ClockCorrection last;
    for (int second = 1; second <= 100; ++second) {
        const auto master = std::chrono::seconds{second};
        ASSERT_TRUE(clock->observe(microphone, at(master), at(master)).hasValue());
        auto observed = clock->observe(
            system, at(master + std::chrono::milliseconds{second * 5}), at(master));
        ASSERT_TRUE(observed.hasValue());
        last = observed.value();
    }
    EXPECT_DOUBLE_EQ(last.audioRateRatio, 0.999);
}

TEST(ClockCoordinatorTest, DelayedFollowerCallbacksNeverMoveCorrectedTimeBackward) {
    auto clock = coordinator();
    const auto microphone = SourceId::create("microphone").value();
    const auto system = SourceId::create("system").value();
    ASSERT_TRUE(clock->observe(microphone, at({}), at({})).hasValue());
    ASSERT_TRUE(clock->observe(system, at({}), at({})).hasValue());

    ASSERT_TRUE(clock->observe(microphone, at(std::chrono::milliseconds{10}),
                               at(std::chrono::seconds{1}))
                    .hasValue());
    const auto delayed = clock->observe(
        system, at(std::chrono::milliseconds{10}),
        at(std::chrono::milliseconds{1'100}));
    ASSERT_TRUE(delayed.hasValue());

    ASSERT_TRUE(clock->observe(microphone, at(std::chrono::milliseconds{20}),
                               at(std::chrono::seconds{2}))
                    .hasValue());
    const auto recovered = clock->observe(
        system, at(std::chrono::milliseconds{20}), at(std::chrono::seconds{2}));
    ASSERT_TRUE(recovered.hasValue());

    EXPECT_GE(recovered.value().correctedTimestamp,
              delayed.value().correctedTimestamp);
}

TEST(ClockCoordinatorTest, RejectsUnknownAndBackwardSourceObservations) {
    auto clock = coordinator();
    const auto microphone = SourceId::create("microphone").value();
    const auto unknown = SourceId::create("unknown").value();

    EXPECT_FALSE(clock->observe(unknown, at({}), at({})).hasValue());
    ASSERT_TRUE(clock->observe(microphone, at(std::chrono::seconds{2}),
                               at(std::chrono::seconds{2}))
                    .hasValue());
    EXPECT_FALSE(clock->observe(microphone, at(std::chrono::seconds{1}),
                                at(std::chrono::seconds{3}))
                     .hasValue());
    EXPECT_FALSE(clock->observe(microphone, at(std::chrono::seconds{3}),
                                at(std::chrono::seconds{1}))
                     .hasValue());
}

TEST(ClockCoordinatorTest, OverflowingObservationDoesNotMutateSourceState) {
    auto clock = coordinator();
    const auto microphone = SourceId::create("microphone").value();
    const auto system = SourceId::create("system").value();
    const auto maximum = at(std::chrono::nanoseconds{
        std::numeric_limits<std::int64_t>::max()});
    ASSERT_TRUE(clock->observe(microphone, maximum, at({})).hasValue());

    EXPECT_FALSE(clock->observe(system, maximum, at(std::chrono::nanoseconds{1}))
                     .hasValue());
    const auto valid = clock->observe(system, maximum, at({}));

    ASSERT_TRUE(valid.hasValue());
    EXPECT_EQ(valid.value().correctedTimestamp, maximum);
    const auto snapshot = clock->snapshot();
    const auto systemState = std::find_if(
        snapshot.sources.begin(), snapshot.sources.end(),
        [&system](const auto& item) { return item.sourceId == system; });
    ASSERT_NE(systemState, snapshot.sources.end());
    EXPECT_EQ(systemState->observationCount, 1u);
}

}  // namespace
