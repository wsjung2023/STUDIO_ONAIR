#include "domain/ProjectManifest.h"

#include "core/Utc.h"
#include "domain/Identifiers.h"

#include <gtest/gtest.h>

namespace {

using creator::core::ErrorCode;
using creator::core::Utc;
using creator::domain::CanvasSettings;
using creator::domain::ManifestColorSpace;
using creator::domain::parseColorSpace;
using creator::domain::ProjectId;
using creator::domain::ProjectManifest;
using creator::domain::validate;

ProjectManifest makeValidManifest() {
    const auto id = ProjectId::create("123e4567-e89b-42d3-a456-426614174000");
    const auto created = Utc::parseRfc3339("2026-07-16T09:30:00Z");
    return ProjectManifest{
        .schemaVersion = ProjectManifest::kCurrentSchemaVersion,
        .projectId = id.value(),
        .name = "MyTutorial",
        .createdAt = created.value(),
        .updatedAt = created.value(),
        .canvas = CanvasSettings{},
        .database = std::string{ProjectManifest::kDatabaseFileName},
        .directories = {},
        .requiredFeatures = {},
    };
}

TEST(ProjectManifestTest, DatabaseNameMatchesSchemaConst) {
    // The only default the schema actually fixes: project.schema.json declares
    // database as {"const": "project.db"}. The canvas numbers are ours to pick -
    // the schema only bounds them - so asserting them here would just read back
    // an initialiser we wrote, which is worth nothing.
    EXPECT_EQ(makeValidManifest().database, "project.db");
}

TEST(ProjectManifestTest, DefaultDirectoriesMatchPackageLayout) {
    const ProjectManifest manifest = makeValidManifest();

    EXPECT_EQ(manifest.directories.media, "media");
    EXPECT_EQ(manifest.directories.audio, "audio");
    EXPECT_EQ(manifest.directories.telemetry, "telemetry");
    EXPECT_EQ(manifest.directories.proxies, "proxies");
    EXPECT_EQ(manifest.directories.thumbnails, "thumbnails");
    EXPECT_EQ(manifest.directories.autosave, "autosave");
    EXPECT_EQ(manifest.directories.renders, "renders");
    EXPECT_EQ(manifest.directories.logs, "logs");
}

TEST(ProjectManifestTest, AcceptsValidManifest) {
    const auto result = validate(makeValidManifest());

    EXPECT_TRUE(result.hasValue());
}

TEST(ProjectManifestTest, RejectsEmptyName) {
    ProjectManifest manifest = makeValidManifest();
    manifest.name = "";

    const auto result = validate(manifest);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(ProjectManifestTest, RejectsOverlongName) {
    ProjectManifest manifest = makeValidManifest();
    manifest.name = std::string(201, 'x');  // schema caps maxLength at 200

    const auto result = validate(manifest);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(ProjectManifestTest, AcceptsNameAtMaxLength) {
    ProjectManifest manifest = makeValidManifest();
    manifest.name = std::string(200, 'x');

    EXPECT_TRUE(validate(manifest).hasValue());
}

TEST(ProjectManifestTest, MeasuresNameInCodePointsNotBytes) {
    // 200 Hangul syllables is 600 UTF-8 bytes but exactly 200 characters, which
    // the schema allows. Counting bytes here would reject it at 67 characters.
    ProjectManifest manifest = makeValidManifest();
    std::string name;
    for (int i = 0; i < 200; ++i) {
        name += "가";
    }
    manifest.name = name;

    ASSERT_EQ(name.size(), 600u) << "fixture must be multi-byte for this test to mean anything";
    EXPECT_TRUE(validate(manifest).hasValue());
}

TEST(ProjectManifestTest, RejectsOverlongUnicodeName) {
    ProjectManifest manifest = makeValidManifest();
    std::string name;
    for (int i = 0; i < 201; ++i) {
        name += "가";
    }
    manifest.name = name;

    const auto result = validate(manifest);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(ProjectManifestTest, RejectsNonUuidProjectId) {
    ProjectManifest manifest = makeValidManifest();
    manifest.projectId = ProjectId::create("not-a-uuid").value();

    const auto result = validate(manifest);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(ProjectManifestTest, RejectsFutureSchemaVersion) {
    ProjectManifest manifest = makeValidManifest();
    manifest.schemaVersion = ProjectManifest::kCurrentSchemaVersion + 1;

    const auto result = validate(manifest);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::UnsupportedVersion);
}

TEST(ProjectManifestTest, RejectsZeroSchemaVersion) {
    ProjectManifest manifest = makeValidManifest();
    manifest.schemaVersion = 0;  // schema requires minimum 1

    const auto result = validate(manifest);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::UnsupportedVersion);
}

TEST(ProjectManifestTest, RejectsWrongDatabaseName) {
    ProjectManifest manifest = makeValidManifest();
    manifest.database = "other.db";  // schema pins this to a const

    const auto result = validate(manifest);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(ProjectManifestTest, RejectsCanvasOutOfSchemaRange) {
    for (const auto [width, height] : {std::pair{15, 1080}, std::pair{1920, 15},
                                       std::pair{16385, 1080}, std::pair{1920, 16385}}) {
        ProjectManifest manifest = makeValidManifest();
        manifest.canvas.width = width;
        manifest.canvas.height = height;

        const auto result = validate(manifest);

        ASSERT_FALSE(result.hasValue()) << width << "x" << height;
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    }
}

TEST(ProjectManifestTest, AcceptsCanvasAtSchemaBounds) {
    for (const auto [width, height] : {std::pair{16, 16}, std::pair{16384, 16384}}) {
        ProjectManifest manifest = makeValidManifest();
        manifest.canvas.width = width;
        manifest.canvas.height = height;

        EXPECT_TRUE(validate(manifest).hasValue()) << width << "x" << height;
    }
}

TEST(ProjectManifestTest, RejectsNonPositiveFrameRate) {
    for (const auto [numerator, denominator] :
         {std::pair<std::int64_t, std::int64_t>{60, 0},
          std::pair<std::int64_t, std::int64_t>{0, 1},
          std::pair<std::int64_t, std::int64_t>{-60, 1},
          std::pair<std::int64_t, std::int64_t>{60, -1}}) {
        ProjectManifest manifest = makeValidManifest();
        manifest.canvas.frameRateNumerator = numerator;
        manifest.canvas.frameRateDenominator = denominator;

        const auto result = validate(manifest);

        ASSERT_FALSE(result.hasValue()) << numerator << "/" << denominator;
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    }
}

TEST(ProjectManifestTest, RejectsEmptyDirectoryName) {
    ProjectManifest manifest = makeValidManifest();
    manifest.directories.media = "";

    const auto result = validate(manifest);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(ProjectManifestTest, RejectsUpdatedBeforeCreated) {
    ProjectManifest manifest = makeValidManifest();
    manifest.createdAt = Utc::parseRfc3339("2026-07-16T09:30:00Z").value();
    manifest.updatedAt = Utc::parseRfc3339("2026-07-16T09:29:59Z").value();

    const auto result = validate(manifest);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(ProjectManifestTest, RejectsDuplicateRequiredFeatures) {
    ProjectManifest manifest = makeValidManifest();
    manifest.requiredFeatures = {"avatar-2d", "avatar-2d"};  // schema: uniqueItems

    const auto result = validate(manifest);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(ColorSpaceTest, RoundTrips) {
    const auto parsed = parseColorSpace(creator::domain::toString(ManifestColorSpace::Rec709Sdr));

    ASSERT_TRUE(parsed.hasValue());
    EXPECT_EQ(parsed.value(), ManifestColorSpace::Rec709Sdr);
}

TEST(ColorSpaceTest, UsesSchemaSpelling) {
    EXPECT_EQ(creator::domain::toString(ManifestColorSpace::Rec709Sdr), "rec709-sdr");
}

TEST(ColorSpaceTest, RejectsUnknownSpelling) {
    const auto parsed = parseColorSpace("rec2020-hdr");

    ASSERT_FALSE(parsed.hasValue());
    EXPECT_EQ(parsed.error().code(), ErrorCode::UnsupportedVersion);
}

}  // namespace
