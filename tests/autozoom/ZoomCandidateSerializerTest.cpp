#include "autozoom/ZoomCandidate.h"
#include "autozoom/ZoomCandidateSerializer.h"
#include "autozoom/ZoomRegion.h"

#include "core/Timebase.h"
#include "cursor/CursorPoint.h"
#include "domain/TimelineTypes.h"

#include "ZoomCandidateSchemaValidator.h"

#include <gtest/gtest.h>

namespace {

using creator::autozoom::ZoomCandidate;
using creator::autozoom::ZoomCandidateSerializer;
using creator::autozoom::ZoomRegion;
using creator::core::DurationNs;
using creator::core::TimestampNs;
using creator::cursor::CursorPoint;
using creator::domain::TimeRange;

ZoomCandidate sample() {
    const auto region = ZoomRegion::create(CursorPoint::create(0.6, 0.4).value(), 2.0).value();
    const auto span = TimeRange::create(TimestampNs{DurationNs{1'000'000}},
                                        DurationNs{2'000'000})
                          .value();
    return ZoomCandidate::create(span, region, 0.8).value();
}

TEST(ZoomCandidateSerializerTest, EmitsSchemaVersionedDocument) {
    const auto json = ZoomCandidateSerializer::toJson(sample());
    ASSERT_TRUE(json.hasValue());
    const auto& doc = json.value();
    EXPECT_EQ(doc.at("schemaVersion").get<int>(), 1);
    EXPECT_EQ(doc.at("span").at("startNs").get<std::int64_t>(), 1'000'000);
    EXPECT_EQ(doc.at("span").at("durationNs").get<std::int64_t>(), 2'000'000);
    EXPECT_DOUBLE_EQ(doc.at("region").at("centerX").get<double>(), 0.6);
    EXPECT_DOUBLE_EQ(doc.at("region").at("zoomFactor").get<double>(), 2.0);
    EXPECT_DOUBLE_EQ(doc.at("score").get<double>(), 0.8);
}

TEST(ZoomCandidateSerializerTest, DocumentValidatesAgainstSchema) {
    const auto json = ZoomCandidateSerializer::toJson(sample());
    ASSERT_TRUE(json.hasValue());
    EXPECT_EQ(autozoom_test::validateCandidate(json.value()), "");
}

}  // namespace
