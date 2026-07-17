#include "domain/TimelineTypes.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <limits>
#include <string>

namespace {

using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::TimestampNs;
using creator::domain::AssetId;
using creator::domain::AudioEnvelope;
using creator::domain::CaptionCue;
using creator::domain::ClipId;
using creator::domain::CueId;
using creator::domain::PipPreset;
using creator::domain::RgbaColor;
using creator::domain::TextAlignment;
using creator::domain::TimeRange;
using creator::domain::TitlePayload;
using creator::domain::VisualTransform;

TimestampNs at(std::int64_t nanoseconds) {
    return TimestampNs{DurationNs{nanoseconds}};
}

static_assert(!std::same_as<AssetId, ClipId>);

TEST(TimeRangeTest, CreatesPositiveHalfOpenRange) {
    const auto created = TimeRange::create(at(10), DurationNs{5});

    ASSERT_TRUE(created.hasValue());
    EXPECT_EQ(created.value().start(), at(10));
    EXPECT_EQ(created.value().duration(), DurationNs{5});
    EXPECT_EQ(created.value().end(), at(15));
}

TEST(TimeRangeTest, RejectsNegativeStartAndNonPositiveDuration) {
    const auto negativeStart = TimeRange::create(at(-1), DurationNs{1});
    const auto zeroDuration = TimeRange::create(at(0), DurationNs::zero());
    const auto negativeDuration = TimeRange::create(at(0), DurationNs{-1});

    ASSERT_FALSE(negativeStart.hasValue());
    EXPECT_EQ(negativeStart.error().code(), ErrorCode::InvalidArgument);
    ASSERT_FALSE(zeroDuration.hasValue());
    EXPECT_EQ(zeroDuration.error().code(), ErrorCode::InvalidArgument);
    ASSERT_FALSE(negativeDuration.hasValue());
    EXPECT_EQ(negativeDuration.error().code(), ErrorCode::InvalidArgument);
}

TEST(TimeRangeTest, RejectsEndOverflow) {
    const auto created = TimeRange::create(
        at(std::numeric_limits<std::int64_t>::max() - 1), DurationNs{2});

    ASSERT_FALSE(created.hasValue());
    EXPECT_EQ(created.error().code(), ErrorCode::InvalidArgument);
}

TEST(TimeRangeTest, UsesHalfOpenOverlapSemantics) {
    const auto first = TimeRange::create(at(10), DurationNs{5}).value();
    const auto touching = TimeRange::create(at(15), DurationNs{3}).value();
    const auto overlapping = TimeRange::create(at(14), DurationNs{3}).value();

    EXPECT_FALSE(overlaps(first, touching));
    EXPECT_FALSE(overlaps(touching, first));
    EXPECT_TRUE(overlaps(first, overlapping));
    EXPECT_TRUE(overlaps(overlapping, first));
}

TEST(VisualTransformTest, CreatesFiniteNormalizedTransform) {
    const auto created = VisualTransform::create(
        0.25, 0.2, 0.5, 0.4, 1.2, 0.8, 15.0,
        0.1, 0.05, 0.2, 0.1, 0.75, 3);

    ASSERT_TRUE(created.hasValue());
    EXPECT_DOUBLE_EQ(created.value().x(), 0.25);
    EXPECT_DOUBLE_EQ(created.value().y(), 0.2);
    EXPECT_DOUBLE_EQ(created.value().width(), 0.5);
    EXPECT_DOUBLE_EQ(created.value().height(), 0.4);
    EXPECT_DOUBLE_EQ(created.value().scaleX(), 1.2);
    EXPECT_DOUBLE_EQ(created.value().scaleY(), 0.8);
    EXPECT_DOUBLE_EQ(created.value().rotationDegrees(), 15.0);
    EXPECT_DOUBLE_EQ(created.value().opacity(), 0.75);
    EXPECT_EQ(created.value().zOrder(), 3);
}

TEST(PipPresetTest, CreatesAndRecognizesEveryCanonicalPreset) {
    for (const auto preset : {PipPreset::FullFrame, PipPreset::TopLeft,
                              PipPreset::TopRight, PipPreset::BottomLeft,
                              PipPreset::BottomRight}) {
        const auto created = creator::domain::visualTransformForPipPreset(
            preset, 16.0 / 9.0, 16.0 / 9.0, 7);
        ASSERT_TRUE(created.hasValue());
        EXPECT_EQ(creator::domain::identifyPipPreset(
                      created.value(), 16.0 / 9.0, 16.0 / 9.0),
                  preset);
        EXPECT_EQ(created.value().zOrder(), 7);
    }
}

TEST(PipPresetTest, PreservesAspectAndUsesSafeMargins) {
    const auto created = creator::domain::visualTransformForPipPreset(
        PipPreset::BottomRight, 4.0 / 3.0, 16.0 / 9.0, 2);

    ASSERT_TRUE(created.hasValue());
    EXPECT_DOUBLE_EQ(created.value().width(), 0.30);
    EXPECT_DOUBLE_EQ(created.value().height(), 0.40);
    EXPECT_DOUBLE_EQ(created.value().x(), 0.66);
    EXPECT_DOUBLE_EQ(created.value().y(), 0.56);
}

TEST(PipPresetTest, ManualChangeBecomesCustomAndBadAspectIsRejected) {
    const auto manual = VisualTransform::create(
        0.04, 0.04, 0.31, 0.30, 1.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 0.0, 1.0, 1).value();

    EXPECT_EQ(creator::domain::identifyPipPreset(
                  manual, 16.0 / 9.0, 16.0 / 9.0),
              PipPreset::Custom);
    EXPECT_FALSE(creator::domain::visualTransformForPipPreset(
                     PipPreset::TopLeft, 0.0, 16.0 / 9.0, 0).hasValue());
    EXPECT_FALSE(creator::domain::visualTransformForPipPreset(
                     PipPreset::Custom, 16.0 / 9.0, 16.0 / 9.0, 0).hasValue());
}

TEST(RgbaColorTest, ParsesAndSerializesOnlyCanonicalRgba) {
    const auto color = RgbaColor::parse("#1a2b3cff");

    ASSERT_TRUE(color.hasValue());
    EXPECT_EQ(color.value().red(), 0x1a);
    EXPECT_EQ(color.value().green(), 0x2b);
    EXPECT_EQ(color.value().blue(), 0x3c);
    EXPECT_EQ(color.value().alpha(), 0xff);
    EXPECT_EQ(color.value().toString(), "#1a2b3cff");
    EXPECT_FALSE(RgbaColor::parse("#1A2B3CFF").hasValue());
    EXPECT_FALSE(RgbaColor::parse("#1a2b3c").hasValue());
    EXPECT_FALSE(RgbaColor::parse("transparent").hasValue());
}

TEST(TitlePayloadTest, ValidatesUnicodeBoundsAndPlacement) {
    const auto foreground = RgbaColor::parse("#ffffffff").value();
    const auto background = RgbaColor::parse("#00000080").value();
    const auto title = TitlePayload::create(
        "크리에이터 Studio", "Malgun Gothic", 0.5, 0.9,
        foreground, background, TextAlignment::Center);

    ASSERT_TRUE(title.hasValue());
    EXPECT_EQ(title.value().text(), "크리에이터 Studio");
    EXPECT_FALSE(TitlePayload::create(
                     "", "Malgun Gothic", 0.5, 0.9,
                     foreground, background, TextAlignment::Center).hasValue());
    EXPECT_FALSE(TitlePayload::create(
                     std::string(513, 'a'), "Malgun Gothic", 0.5, 0.9,
                     foreground, background, TextAlignment::Center).hasValue());
    EXPECT_FALSE(TitlePayload::create(
                     std::string{"\xC0\xAF", 2}, "Malgun Gothic", 0.5, 0.9,
                     foreground, background, TextAlignment::Center).hasValue());
    EXPECT_FALSE(TitlePayload::create(
                     std::string{"\xE2\x82", 2}, "Malgun Gothic", 0.5, 0.9,
                     foreground, background, TextAlignment::Center).hasValue());
    EXPECT_FALSE(TitlePayload::create(
                     "text", "", 0.5, 0.9,
                     foreground, background, TextAlignment::Center).hasValue());
    EXPECT_FALSE(TitlePayload::create(
                     "text", "font", 1.1, 0.9,
                     foreground, background, TextAlignment::Center).hasValue());
    EXPECT_FALSE(TitlePayload::create(
                     "text", "font", 0.5, 0.9, foreground, background,
                     static_cast<TextAlignment>(99)).hasValue());
}

TEST(CaptionCueTest, ValidatesUnicodeAndCheckedPositiveRange) {
    const auto cue = CaptionCue::create(
        CueId::create("cue-1").value(), DurationNs{10}, DurationNs{20},
        "안녕하세요");

    ASSERT_TRUE(cue.hasValue());
    EXPECT_EQ(cue.value().endOffset(), DurationNs{30});
    EXPECT_FALSE(CaptionCue::create(
                     CueId::create("cue-2").value(), DurationNs{-1},
                     DurationNs{20}, "bad").hasValue());
    EXPECT_FALSE(CaptionCue::create(
                     CueId::create("cue-3").value(), DurationNs{0},
                     DurationNs{0}, "bad").hasValue());
    EXPECT_FALSE(CaptionCue::create(
                     CueId::create("cue-4").value(), DurationNs{0},
                     DurationNs{1}, std::string(2001, 'a')).hasValue());
}

TEST(VisualTransformTest, RejectsInvalidBoundsAndNonFiniteValues) {
    const auto zeroWidth = VisualTransform::create(
        0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 0.0, 1.0, 0);
    const auto zeroScale = VisualTransform::create(
        0.0, 0.0, 1.0, 1.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 0.0, 1.0, 0);
    const auto excessiveCrop = VisualTransform::create(
        0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0,
        0.6, 0.0, 0.4, 0.0, 1.0, 0);
    const auto excessiveOpacity = VisualTransform::create(
        0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 0.0, 1.1, 0);
    const auto nanRotation = VisualTransform::create(
        0.0, 0.0, 1.0, 1.0, 1.0, 1.0,
        std::numeric_limits<double>::quiet_NaN(),
        0.0, 0.0, 0.0, 0.0, 1.0, 0);

    EXPECT_FALSE(zeroWidth.hasValue());
    EXPECT_FALSE(zeroScale.hasValue());
    EXPECT_FALSE(excessiveCrop.hasValue());
    EXPECT_FALSE(excessiveOpacity.hasValue());
    EXPECT_FALSE(nanRotation.hasValue());
}

TEST(AudioEnvelopeTest, CreatesBoundedGainAndFades) {
    const auto created = AudioEnvelope::create(
        -6.0, DurationNs{1}, DurationNs{2}, DurationNs{10});

    ASSERT_TRUE(created.hasValue());
    EXPECT_DOUBLE_EQ(created.value().gainDb(), -6.0);
    EXPECT_EQ(created.value().fadeIn(), DurationNs{1});
    EXPECT_EQ(created.value().fadeOut(), DurationNs{2});
}

TEST(AudioEnvelopeTest, RejectsInvalidGainAndFadeDurations) {
    const auto excessiveGain = AudioEnvelope::create(
        24.1, DurationNs::zero(), DurationNs::zero(), DurationNs{1});
    const auto negativeFade = AudioEnvelope::create(
        0.0, DurationNs{-1}, DurationNs::zero(), DurationNs{1});
    const auto overlappingFades = AudioEnvelope::create(
        0.0, DurationNs{6}, DurationNs{5}, DurationNs{10});
    const auto nanGain = AudioEnvelope::create(
        std::numeric_limits<double>::quiet_NaN(), DurationNs::zero(),
        DurationNs::zero(), DurationNs{1});

    EXPECT_FALSE(excessiveGain.hasValue());
    EXPECT_FALSE(negativeFade.hasValue());
    EXPECT_FALSE(overlappingFades.hasValue());
    EXPECT_FALSE(nanGain.hasValue());
}

}  // namespace
