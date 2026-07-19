#include "cursor_emphasis/ClickEmphasis.h"
#include "cursor_emphasis/CursorHideSpan.h"
#include "cursor_emphasis/EmphasisPlan.h"
#include "cursor_emphasis/EmphasisPlanSerializer.h"

#include "core/Timebase.h"
#include "cursor/CursorButton.h"
#include "cursor/CursorPoint.h"
#include "domain/TimelineTypes.h"

#include "EmphasisPlanSchemaValidator.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using creator::core::DurationNs;
using creator::core::TimestampNs;
using creator::cursor::CursorButton;
using creator::cursor::CursorPoint;
using creator::cursor_emphasis::ClickEmphasis;
using creator::cursor_emphasis::CursorHideSpan;
using creator::cursor_emphasis::EmphasisPlan;
using creator::cursor_emphasis::EmphasisPlanSerializer;
using creator::cursor_emphasis::EmphasisStyle;
using creator::cursor_emphasis::HideReason;
using creator::domain::TimeRange;

EmphasisPlan sample() {
    auto click = ClickEmphasis::create(CursorPoint::create(0.3, 0.7).value(),
                                       TimestampNs{DurationNs{1'000'000}}, DurationNs{600'000'000},
                                       CursorButton::Right, EmphasisStyle::Highlight, 0.06)
                     .value();
    auto hide = CursorHideSpan::create(
                    TimeRange::create(TimestampNs{DurationNs{2'000'000}}, DurationNs{3'000'000})
                        .value(),
                    HideReason::Idle)
                    .value();
    return EmphasisPlan::create({click}, {hide}).value();
}

TEST(EmphasisPlanSerializerTest, EmitsSchemaVersionedDocument) {
    const auto json = EmphasisPlanSerializer::toJson(sample());
    ASSERT_TRUE(json.hasValue());
    const auto& doc = json.value();
    EXPECT_EQ(doc.at("schemaVersion").get<int>(), 1);

    const auto& click = doc.at("clicks").at(0);
    EXPECT_EQ(click.at("startNs").get<std::int64_t>(), 1'000'000);
    EXPECT_EQ(click.at("durationNs").get<std::int64_t>(), 600'000'000);
    EXPECT_DOUBLE_EQ(click.at("x").get<double>(), 0.3);
    EXPECT_EQ(click.at("button").get<std::string>(), "right");
    EXPECT_EQ(click.at("style").get<std::string>(), "highlight");

    const auto& hide = doc.at("hideSpans").at(0);
    EXPECT_EQ(hide.at("startNs").get<std::int64_t>(), 2'000'000);
    EXPECT_EQ(hide.at("durationNs").get<std::int64_t>(), 3'000'000);
    EXPECT_EQ(hide.at("reason").get<std::string>(), "idle");
}

TEST(EmphasisPlanSerializerTest, DocumentValidatesAgainstSchema) {
    const auto json = EmphasisPlanSerializer::toJson(sample());
    ASSERT_TRUE(json.hasValue());
    EXPECT_EQ(cursor_emphasis_test::validatePlan(json.value()), "");
}

TEST(EmphasisPlanSerializerTest, EmptyPlanValidatesAgainstSchema) {
    const auto json = EmphasisPlanSerializer::toJson(EmphasisPlan::create({}, {}).value());
    ASSERT_TRUE(json.hasValue());
    EXPECT_EQ(cursor_emphasis_test::validatePlan(json.value()), "");
}

}  // namespace
