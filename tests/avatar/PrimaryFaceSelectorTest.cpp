#include "avatar/PrimaryFaceSelector.h"

#include <array>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "avatar/TrackingResult.h"
#include "core/Timebase.h"

namespace {

using creator::avatar::selectPrimaryFace;
using creator::avatar::TrackingResult;
using creator::core::DurationNs;
using creator::core::TimestampNs;

TrackingResult makeResult(float confidence, bool faceFound, std::int64_t timestampNs = 0) {
    TrackingResult result{};
    result.timestamp = TimestampNs{DurationNs{timestampNs}};
    result.confidence = confidence;
    result.faceFound = faceFound;
    return result;
}

TEST(PrimaryFaceSelectorTest, PicksHigherConfidenceOfTwoFaceFoundResults) {
    const std::array<TrackingResult, 2> candidates{
        makeResult(0.4F, true, /*timestampNs=*/1),
        makeResult(0.9F, true, /*timestampNs=*/2),
    };

    const std::optional<TrackingResult> selected = selectPrimaryFace(candidates);

    ASSERT_TRUE(selected.has_value());
    EXPECT_FLOAT_EQ(selected->confidence, 0.9F);
    EXPECT_EQ(selected->timestamp, TimestampNs{DurationNs{2}});
}

TEST(PrimaryFaceSelectorTest, SkipsANonFaceEvenWithHigherConfidence) {
    // The !faceFound candidate has the higher raw confidence number, but a
    // non-face must never be selected regardless of its confidence value.
    const std::array<TrackingResult, 2> candidates{
        makeResult(0.95F, /*faceFound=*/false, /*timestampNs=*/1),
        makeResult(0.2F, /*faceFound=*/true, /*timestampNs=*/2),
    };

    const std::optional<TrackingResult> selected = selectPrimaryFace(candidates);

    ASSERT_TRUE(selected.has_value());
    EXPECT_TRUE(selected->faceFound);
    EXPECT_FLOAT_EQ(selected->confidence, 0.2F);
    EXPECT_EQ(selected->timestamp, TimestampNs{DurationNs{2}});
}

TEST(PrimaryFaceSelectorTest, AllLostReturnsNullopt) {
    const std::array<TrackingResult, 3> candidates{
        makeResult(0.9F, /*faceFound=*/false),
        makeResult(0.5F, /*faceFound=*/false),
        makeResult(0.1F, /*faceFound=*/false),
    };

    const std::optional<TrackingResult> selected = selectPrimaryFace(candidates);

    EXPECT_FALSE(selected.has_value());
}

TEST(PrimaryFaceSelectorTest, EmptySpanReturnsNullopt) {
    const std::vector<TrackingResult> candidates;

    const std::optional<TrackingResult> selected = selectPrimaryFace(candidates);

    EXPECT_FALSE(selected.has_value());
}

TEST(PrimaryFaceSelectorTest, TieInConfidencePicksTheFirstCandidate) {
    // Equal confidence on both faceFound candidates; distinguish them by
    // timestamp so the assertion pins which one was actually returned rather
    // than just re-reading a value both share.
    const std::array<TrackingResult, 2> candidates{
        makeResult(0.6F, true, /*timestampNs=*/111),
        makeResult(0.6F, true, /*timestampNs=*/222),
    };

    const std::optional<TrackingResult> selected = selectPrimaryFace(candidates);

    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->timestamp, TimestampNs{DurationNs{111}})
        << "tie must resolve to the first candidate in span order";
}

TEST(PrimaryFaceSelectorTest, SingleFaceFoundCandidateIsReturned) {
    const std::array<TrackingResult, 1> candidates{
        makeResult(0.5F, true, /*timestampNs=*/7),
    };

    const std::optional<TrackingResult> selected = selectPrimaryFace(candidates);

    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->timestamp, TimestampNs{DurationNs{7}});
    EXPECT_FLOAT_EQ(selected->confidence, 0.5F);
}

}  // namespace
