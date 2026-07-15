#include "core/Utc.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using creator::core::ErrorCode;
using creator::core::Utc;

TEST(UtcTest, RoundTripsRfc3339) {
    const auto parsed = Utc::parseRfc3339("2026-07-16T09:30:00Z");

    ASSERT_TRUE(parsed.hasValue());
    EXPECT_EQ(parsed.value().toRfc3339(), "2026-07-16T09:30:00Z");
}

TEST(UtcTest, ParsesEpoch) {
    const auto parsed = Utc::parseRfc3339("1970-01-01T00:00:00Z");

    ASSERT_TRUE(parsed.hasValue());
    EXPECT_EQ(parsed.value().timePoint().time_since_epoch().count(), 0);
    EXPECT_EQ(parsed.value().toRfc3339(), "1970-01-01T00:00:00Z");
}

TEST(UtcTest, ParsesLeapDay) {
    const auto parsed = Utc::parseRfc3339("2024-02-29T23:59:59Z");

    ASSERT_TRUE(parsed.hasValue());
    EXPECT_EQ(parsed.value().toRfc3339(), "2024-02-29T23:59:59Z");
}

TEST(UtcTest, NowIsFormattableAndReparsable) {
    const Utc now = Utc::now();
    const std::string text = now.toRfc3339();

    ASSERT_EQ(text.size(), 20u);
    EXPECT_EQ(text[4], '-');
    EXPECT_EQ(text[7], '-');
    EXPECT_EQ(text[10], 'T');
    EXPECT_EQ(text[13], ':');
    EXPECT_EQ(text[16], ':');
    EXPECT_EQ(text[19], 'Z');

    const auto reparsed = Utc::parseRfc3339(text);
    ASSERT_TRUE(reparsed.hasValue());
    EXPECT_EQ(reparsed.value(), now);
}

TEST(UtcTest, Orders) {
    const auto earlier = Utc::parseRfc3339("2026-07-16T09:30:00Z");
    const auto later = Utc::parseRfc3339("2026-07-16T09:30:01Z");

    ASSERT_TRUE(earlier.hasValue());
    ASSERT_TRUE(later.hasValue());
    EXPECT_LT(earlier.value(), later.value());
    EXPECT_NE(earlier.value(), later.value());
}

TEST(UtcTest, RejectsMalformedInput) {
    for (const char* bad : {
             "",
             "2026-07-16",                 // date only
             "2026-07-16T09:30:00",        // no zone
             "2026-07-16T09:30:00+09:00",  // offset, not UTC
             "2026-07-16 09:30:00Z",       // space instead of T
             "2026-13-16T09:30:00Z",       // month 13
             "2026-07-32T09:30:00Z",       // day 32
             "2026-02-30T09:30:00Z",       // not a real date
             "2026-07-16T24:00:00Z",       // hour 24
             "2026-07-16T09:60:00Z",       // minute 60
             "2026-07-16T09:30:60Z",       // second 60
             "20xx-07-16T09:30:00Z",       // non-digit
         }) {
        const auto parsed = Utc::parseRfc3339(bad);
        EXPECT_FALSE(parsed.hasValue()) << "should have rejected: " << bad;
        if (!parsed.hasValue()) {
            EXPECT_EQ(parsed.error().code(), ErrorCode::ParseFailure) << bad;
        }
    }
}

}  // namespace
