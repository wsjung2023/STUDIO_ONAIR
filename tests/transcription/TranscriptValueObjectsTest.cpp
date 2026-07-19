#include "transcription/Transcript.h"
#include "transcription/TranscriptSegment.h"
#include "transcription/TranscriptWord.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/TimelineTypes.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

using creator::core::ErrorCode;
using creator::domain::SourceId;
using creator::domain::TimeRange;
using creator::transcription::Transcript;
using creator::transcription::TranscriptSegment;
using creator::transcription::TranscriptWord;

TimeRange range(std::int64_t startNs, std::int64_t durNs) {
    return TimeRange::create(creator::core::TimestampNs{creator::core::DurationNs{startNs}},
                             creator::core::DurationNs{durNs})
        .value();
}

SourceId source() { return SourceId::create("cam-1").value(); }

TranscriptWord word(std::string text, std::int64_t startNs, std::int64_t durNs,
                    double confidence) {
    return TranscriptWord::create(std::move(text), range(startNs, durNs), confidence).value();
}

// ---- TranscriptWord ----

TEST(TranscriptWordTest, AcceptsValidWord) {
    const auto result = TranscriptWord::create("hello", range(0, 100), 0.5);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().text(), "hello");
    EXPECT_DOUBLE_EQ(result.value().confidence(), 0.5);
}

TEST(TranscriptWordTest, RejectsEmptyText) {
    const auto result = TranscriptWord::create("", range(0, 100), 0.5);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(TranscriptWordTest, RejectsInvalidUtf8Text) {
    const auto result = TranscriptWord::create(std::string{"\xff\xfe"}, range(0, 100), 0.5);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(TranscriptWordTest, AcceptsBoundaryConfidence) {
    EXPECT_TRUE(TranscriptWord::create("a", range(0, 100), 0.0).hasValue());
    EXPECT_TRUE(TranscriptWord::create("a", range(0, 100), 1.0).hasValue());
}

TEST(TranscriptWordTest, RejectsConfidenceOutOfRange) {
    EXPECT_FALSE(TranscriptWord::create("a", range(0, 100), -0.01).hasValue());
    EXPECT_FALSE(TranscriptWord::create("a", range(0, 100), 1.01).hasValue());
}

TEST(TranscriptWordTest, RejectsNonFiniteConfidence) {
    EXPECT_FALSE(
        TranscriptWord::create("a", range(0, 100),
                               std::numeric_limits<double>::quiet_NaN())
            .hasValue());
    EXPECT_FALSE(
        TranscriptWord::create("a", range(0, 100),
                               std::numeric_limits<double>::infinity())
            .hasValue());
}

// TimeRange itself rejects negative timestamps; confirm the invariant we rely on.
TEST(TranscriptWordTest, RejectsNegativeStartViaTimeRange) {
    const auto badRange = TimeRange::create(
        creator::core::TimestampNs{creator::core::DurationNs{-1}},
        creator::core::DurationNs{100});
    EXPECT_FALSE(badRange.hasValue());
}

// ---- TranscriptSegment ----

TEST(TranscriptSegmentTest, AcceptsMonotonicWordsInRange) {
    std::vector<TranscriptWord> words{word("the", 0, 400, 0.9),
                                      word("fox", 500, 400, 0.8)};
    const auto result = TranscriptSegment::create("the fox", range(0, 1000), std::move(words));
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().words().size(), 2u);
    EXPECT_FALSE(result.value().speaker().has_value());
}

TEST(TranscriptSegmentTest, AcceptsEmptyWordList) {
    EXPECT_TRUE(TranscriptSegment::create("silence", range(0, 1000), {}).hasValue());
}

TEST(TranscriptSegmentTest, AcceptsSpeakerLabel) {
    const auto result = TranscriptSegment::create(
        "hi", range(0, 1000), {}, std::optional<std::string>{"S1"});
    ASSERT_TRUE(result.hasValue());
    ASSERT_TRUE(result.value().speaker().has_value());
    EXPECT_EQ(*result.value().speaker(), "S1");
}

TEST(TranscriptSegmentTest, RejectsEmptyText) {
    EXPECT_FALSE(TranscriptSegment::create("", range(0, 1000), {}).hasValue());
}

TEST(TranscriptSegmentTest, RejectsInvalidUtf8Text) {
    EXPECT_FALSE(
        TranscriptSegment::create(std::string{"\xff"}, range(0, 1000), {}).hasValue());
}

TEST(TranscriptSegmentTest, RejectsEmptySpeakerLabel) {
    EXPECT_FALSE(TranscriptSegment::create("hi", range(0, 1000), {},
                                           std::optional<std::string>{""})
                     .hasValue());
}

TEST(TranscriptSegmentTest, RejectsWordStartingBeforeSegment) {
    std::vector<TranscriptWord> words{word("x", 0, 400, 0.9)};
    const auto result =
        TranscriptSegment::create("x", range(100, 900), std::move(words));
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(TranscriptSegmentTest, RejectsWordEndingAfterSegment) {
    std::vector<TranscriptWord> words{word("x", 800, 400, 0.9)};  // ends at 1200
    const auto result = TranscriptSegment::create("x", range(0, 1000), std::move(words));
    ASSERT_FALSE(result.hasValue());
}

TEST(TranscriptSegmentTest, RejectsOverlappingWords) {
    std::vector<TranscriptWord> words{word("a", 0, 600, 0.9),
                                      word("b", 400, 400, 0.9)};  // starts before a ends
    const auto result = TranscriptSegment::create("a b", range(0, 1000), std::move(words));
    ASSERT_FALSE(result.hasValue());
}

TEST(TranscriptSegmentTest, RejectsNonMonotonicWords) {
    std::vector<TranscriptWord> words{word("b", 500, 400, 0.9),
                                      word("a", 0, 400, 0.9)};  // out of order
    const auto result = TranscriptSegment::create("b a", range(0, 1000), std::move(words));
    ASSERT_FALSE(result.hasValue());
}

// ---- Transcript ----

TranscriptSegment segment(std::string text, std::int64_t startNs, std::int64_t durNs) {
    return TranscriptSegment::create(std::move(text), range(startNs, durNs), {}).value();
}

TEST(TranscriptTest, AcceptsMonotonicSegments) {
    std::vector<TranscriptSegment> segments{segment("one", 0, 1000),
                                            segment("two", 1000, 1000)};
    const auto result = Transcript::create(std::move(segments), "en", source());
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().segments().size(), 2u);
    EXPECT_EQ(result.value().languageTag(), "en");
    EXPECT_EQ(result.value().sourceId().value(), "cam-1");
}

TEST(TranscriptTest, AcceptsEmptySegments) {
    EXPECT_TRUE(Transcript::create({}, "ko", source()).hasValue());
}

TEST(TranscriptTest, AcceptsRegionLanguageTag) {
    EXPECT_TRUE(Transcript::create({}, "en-US", source()).hasValue());
}

TEST(TranscriptTest, RejectsEmptyLanguageTag) {
    EXPECT_FALSE(Transcript::create({}, "", source()).hasValue());
}

TEST(TranscriptTest, RejectsMalformedLanguageTag) {
    EXPECT_FALSE(Transcript::create({}, "e", source()).hasValue());        // too short
    EXPECT_FALSE(Transcript::create({}, "en_US", source()).hasValue());    // underscore
    EXPECT_FALSE(Transcript::create({}, "123", source()).hasValue());      // not alpha primary
    EXPECT_FALSE(Transcript::create({}, "en-", source()).hasValue());      // trailing dash
}

TEST(TranscriptTest, RejectsOverlappingSegments) {
    std::vector<TranscriptSegment> segments{segment("one", 0, 1200),
                                            segment("two", 1000, 1000)};
    const auto result = Transcript::create(std::move(segments), "en", source());
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(TranscriptTest, RejectsNonMonotonicSegments) {
    std::vector<TranscriptSegment> segments{segment("two", 1000, 1000),
                                            segment("one", 0, 1000)};
    EXPECT_FALSE(Transcript::create(std::move(segments), "en", source()).hasValue());
}

TEST(TranscriptTest, SegmentAtFindsContainingSegment) {
    std::vector<TranscriptSegment> segments{segment("one", 0, 1000),
                                            segment("two", 1000, 1000)};
    const auto transcript = Transcript::create(std::move(segments), "en", source()).value();

    const auto at = [&](std::int64_t ns) {
        return transcript.segmentAt(
            creator::core::TimestampNs{creator::core::DurationNs{ns}});
    };
    ASSERT_NE(at(500), nullptr);
    EXPECT_EQ(at(500)->text(), "one");
    ASSERT_NE(at(1500), nullptr);
    EXPECT_EQ(at(1500)->text(), "two");
    EXPECT_EQ(at(5000), nullptr);  // past the end
}

TEST(TranscriptTest, FullTextJoinsSegments) {
    std::vector<TranscriptSegment> segments{segment("hello", 0, 1000),
                                            segment("world", 1000, 1000)};
    const auto transcript = Transcript::create(std::move(segments), "en", source()).value();
    EXPECT_EQ(transcript.fullText(), "hello world");
}

}  // namespace
