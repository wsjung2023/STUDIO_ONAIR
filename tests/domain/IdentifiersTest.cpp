#include "domain/Identifiers.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <type_traits>

namespace {

using creator::core::ErrorCode;
using creator::domain::ProjectId;
using creator::domain::CueId;
using creator::domain::SessionId;
using creator::domain::SourceId;

// The whole point of tagged identifiers: passing a ProjectId where a SourceId is
// expected must not compile. If these ever start passing, the type safety in
// "매직 문자열 대신 typed ID/value object 사용" (CLAUDE.md 4) is gone.
static_assert(!std::is_same_v<ProjectId, SourceId>);
static_assert(!std::is_same_v<ProjectId, SessionId>);
static_assert(!std::is_same_v<SourceId, SessionId>);
static_assert(!std::is_same_v<CueId, SourceId>);
static_assert(!std::is_convertible_v<ProjectId, SourceId>);
static_assert(!std::is_convertible_v<SourceId, ProjectId>);
static_assert(!std::is_constructible_v<SourceId, ProjectId>);

// A default-constructible id would be an id with no value - an invalid state
// that every consumer would then have to check for.
static_assert(!std::is_default_constructible_v<ProjectId>);
static_assert(!std::is_default_constructible_v<SourceId>);
static_assert(!std::is_default_constructible_v<SessionId>);
static_assert(!std::is_default_constructible_v<CueId>);

// Ids are copied into domain structs constantly; they must stay cheap to move.
static_assert(std::is_nothrow_move_constructible_v<ProjectId>);

TEST(IdentifiersTest, CreatesFromNonEmptyString) {
    const auto id = SourceId::create("screen-1");

    ASSERT_TRUE(id.hasValue());
    EXPECT_EQ(id.value().value(), "screen-1");
}

TEST(IdentifiersTest, SerializesToString) {
    const auto id = SourceId::create("screen-1");

    ASSERT_TRUE(id.hasValue());
    EXPECT_EQ(id.value().toString(), "screen-1");
}

TEST(IdentifiersTest, RoundTripsThroughString) {
    const auto original = ProjectId::create("123e4567-e89b-42d3-a456-426614174000");
    ASSERT_TRUE(original.hasValue());

    const std::string serialized = original.value().toString();
    const auto restored = ProjectId::create(serialized);

    ASSERT_TRUE(restored.hasValue());
    EXPECT_EQ(restored.value(), original.value());
}

TEST(IdentifiersTest, RejectsEmptyString) {
    const auto id = SourceId::create("");

    ASSERT_FALSE(id.hasValue());
    EXPECT_EQ(id.error().code(), ErrorCode::InvalidArgument);
}

TEST(IdentifiersTest, CreatesTypedCaptionCueIdentity) {
    const auto cue = CueId::create("caption-cue-1");

    ASSERT_TRUE(cue.hasValue());
    EXPECT_EQ(cue.value().value(), "caption-cue-1");
}

TEST(IdentifiersTest, ComparesByValue) {
    const auto a = SourceId::create("screen-1");
    const auto b = SourceId::create("screen-1");
    const auto c = SourceId::create("camera-1");

    ASSERT_TRUE(a.hasValue());
    ASSERT_TRUE(b.hasValue());
    ASSERT_TRUE(c.hasValue());
    EXPECT_EQ(a.value(), b.value());
    EXPECT_NE(a.value(), c.value());
}

TEST(IdentifiersTest, OrdersForUseAsMapKey) {
    const auto camera = SourceId::create("camera-1");
    const auto screen = SourceId::create("screen-1");
    ASSERT_TRUE(camera.hasValue());
    ASSERT_TRUE(screen.hasValue());

    EXPECT_LT(camera.value(), screen.value());

    std::map<SourceId, int> bySource;
    bySource.emplace(camera.value(), 1);
    bySource.emplace(screen.value(), 2);
    EXPECT_EQ(bySource.size(), 2u);
    EXPECT_EQ(bySource.at(camera.value()), 1);
}

TEST(IdentifiersTest, PreservesUnicodeAndWhitespace) {
    // Ids come from user-named scenes and sources; do not silently normalise.
    const auto id = SourceId::create("화면 1");

    ASSERT_TRUE(id.hasValue());
    EXPECT_EQ(id.value().value(), "화면 1");
}

}  // namespace
