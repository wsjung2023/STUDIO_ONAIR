#include "cursor/CursorClickEvent.h"
#include "cursor/CursorEventSerializer.h"
#include "cursor/CursorMoveEvent.h"
#include "cursor/CursorNormalizer.h"
#include "fakes/FakeCursorSource.h"

#include "core/Timebase.h"
#include "domain/Identifiers.h"

#include "CursorSchemaValidator.h"

#include <gtest/gtest.h>

#include <variant>
#include <vector>

namespace {

using creator::core::DurationNs;
using creator::core::TimestampNs;
using creator::cursor::CursorButton;
using creator::cursor::CursorClickEvent;
using creator::cursor::CursorEventSerializer;
using creator::cursor::CursorMoveEvent;
using creator::cursor::CursorNormalizer;
using creator::cursor::RawCursorClickSample;
using creator::cursor::RawCursorMoveSample;
using creator::cursor::RawCursorSample;
using creator::domain::SourceId;
using creator::fakes::FakeCursorSource;

TimestampNs at(std::int64_t ns) {
    return TimestampNs{DurationNs{ns}};
}

CursorButton buttonFromOrdinal(std::uint8_t ordinal) {
    switch (ordinal) {
        case 1:
            return CursorButton::Right;
        case 2:
            return CursorButton::Middle;
        default:
            return CursorButton::Left;
    }
}

TEST(CursorPipelineTest, FakeSourceThroughNormalizeSerializeValidates) {
    const auto sourceId = SourceId::create("screen-primary").value();
    FakeCursorSource source{{
        RawCursorSample{RawCursorMoveSample{at(0), 0, 0, 1920, 1080}},
        RawCursorSample{RawCursorMoveSample{at(1'000'000), 960, 540, 1920, 1080}},
        RawCursorSample{RawCursorClickSample{at(2'000'000), 960, 540, 1920, 1080, 0}},
        RawCursorSample{RawCursorClickSample{at(3'000'000), 1920, 1080, 1920, 1080, 1}},
    }};

    int moveCount = 0;
    int clickCount = 0;
    while (auto sample = source.poll()) {
        if (const auto* move = std::get_if<RawCursorMoveSample>(&*sample)) {
            const auto point =
                CursorNormalizer::normalize(move->x, move->y, move->sourceWidth, move->sourceHeight);
            ASSERT_TRUE(point.hasValue());
            const auto event = CursorMoveEvent::create(move->tNs, point.value(), sourceId);
            ASSERT_TRUE(event.hasValue());
            const auto json = CursorEventSerializer::toJson(event.value());
            ASSERT_TRUE(json.hasValue());
            EXPECT_EQ(json.value()["type"], "cursor.move");
            EXPECT_TRUE(json.value().contains("sourceId"));
            EXPECT_EQ(cursor_test::validateEvent(json.value()), "")
                << json.value().dump();
            ++moveCount;
        } else {
            const auto* click = std::get_if<RawCursorClickSample>(&*sample);
            ASSERT_NE(click, nullptr);
            const auto point = CursorNormalizer::normalize(click->x, click->y, click->sourceWidth,
                                                           click->sourceHeight);
            ASSERT_TRUE(point.hasValue());
            const auto event = CursorClickEvent::create(click->tNs, point.value(),
                                                        buttonFromOrdinal(click->button));
            ASSERT_TRUE(event.hasValue());
            const auto json = CursorEventSerializer::toJson(event.value());
            ASSERT_TRUE(json.hasValue());
            EXPECT_EQ(json.value()["type"], "cursor.click");
            EXPECT_TRUE(json.value().contains("button"));
            EXPECT_EQ(cursor_test::validateEvent(json.value()), "")
                << json.value().dump();
            ++clickCount;
        }
    }

    EXPECT_EQ(moveCount, 2);
    EXPECT_EQ(clickCount, 2);
}

TEST(CursorPipelineTest, MoveEventJsonHasCanonicalShape) {
    const auto sourceId = SourceId::create("screen-primary").value();
    const auto point = CursorNormalizer::normalize(480, 270, 1920, 1080).value();
    const auto event = CursorMoveEvent::create(at(1'234), point, sourceId).value();
    const auto json = CursorEventSerializer::toJson(event).value();

    EXPECT_EQ(json["tNs"], 1'234);
    EXPECT_EQ(json["type"], "cursor.move");
    EXPECT_DOUBLE_EQ(json["x"].get<double>(), 0.25);
    EXPECT_DOUBLE_EQ(json["y"].get<double>(), 0.25);
    EXPECT_EQ(json["sourceId"], "screen-primary");
    EXPECT_EQ(cursor_test::validateEvent(json), "");
}

TEST(CursorPipelineTest, ClickEventJsonValidatesForEveryButton) {
    const auto point = CursorNormalizer::normalize(0, 0, 100, 100).value();
    for (const auto button : {CursorButton::Left, CursorButton::Right, CursorButton::Middle}) {
        const auto event = CursorClickEvent::create(at(10), point, button).value();
        const auto json = CursorEventSerializer::toJson(event).value();
        EXPECT_EQ(json["type"], "cursor.click");
        EXPECT_EQ(cursor_test::validateEvent(json), "") << json.dump();
    }
}

}  // namespace
