#include "core/Uuid.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace {

using creator::core::generateUuidV4;
using creator::core::isUuidV4;

TEST(UuidTest, GeneratesCanonicalForm) {
    const std::string uuid = generateUuidV4();

    ASSERT_EQ(uuid.size(), 36u);
    EXPECT_EQ(uuid[8], '-');
    EXPECT_EQ(uuid[13], '-');
    EXPECT_EQ(uuid[18], '-');
    EXPECT_EQ(uuid[23], '-');
    EXPECT_TRUE(isUuidV4(uuid)) << uuid;
}

TEST(UuidTest, SetsVersionAndVariantBits) {
    for (int i = 0; i < 100; ++i) {
        const std::string uuid = generateUuidV4();
        EXPECT_EQ(uuid[14], '4') << uuid;                    // version 4
        const char variant = uuid[19];
        EXPECT_TRUE(variant == '8' || variant == '9' || variant == 'a' || variant == 'b')
            << uuid;                                          // RFC 4122 variant
    }
}

TEST(UuidTest, GeneratesDistinctValues) {
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) {
        seen.insert(generateUuidV4());
    }
    EXPECT_EQ(seen.size(), 1000u);
}

TEST(UuidTest, RejectsMalformedInput) {
    EXPECT_FALSE(isUuidV4(""));
    EXPECT_FALSE(isUuidV4("not-a-uuid"));
    EXPECT_FALSE(isUuidV4("123e4567-e89b-12d3-a456-426614174000"));   // version 1
    EXPECT_FALSE(isUuidV4("123e4567e89b42d3a456426614174000"));       // no dashes
    EXPECT_FALSE(isUuidV4("123E4567-E89B-42D3-A456-426614174000"));   // uppercase
    EXPECT_FALSE(isUuidV4("123e4567-e89b-42d3-c456-426614174000"));   // bad variant
    EXPECT_FALSE(isUuidV4("123e4567-e89b-42d3-a456-42661417400"));    // too short
    EXPECT_FALSE(isUuidV4("123e4567-e89b-42d3-a456-4266141740000"));  // too long
    EXPECT_FALSE(isUuidV4("123e4567+e89b-42d3-a456-426614174000"));   // bad separator
    EXPECT_FALSE(isUuidV4("123e4567-e89b-42d3-a456-42661417400g"));   // non-hex
}

TEST(UuidTest, AcceptsKnownGoodV4) {
    EXPECT_TRUE(isUuidV4("123e4567-e89b-42d3-a456-426614174000"));
    EXPECT_TRUE(isUuidV4("00000000-0000-4000-8000-000000000000"));
    EXPECT_TRUE(isUuidV4("ffffffff-ffff-4fff-bfff-ffffffffffff"));
}

}  // namespace
