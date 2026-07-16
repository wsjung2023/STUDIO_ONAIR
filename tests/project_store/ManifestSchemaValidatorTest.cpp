#include "project_store/ManifestSchemaValidator.h"

#include "core/AppError.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <string>
#include <utility>

namespace {

using creator::core::ErrorCode;
using creator::project_store::validateManifestJson;

nlohmann::json validManifest() {
    return nlohmann::json::parse(R"JSON({
      "schemaVersion": 1,
      "projectId": "123e4567-e89b-42d3-a456-426614174000",
      "name": "강의 프로젝트",
      "createdAt": "2026-07-16T09:30:00Z",
      "updatedAt": "2026-07-16T09:30:00Z",
      "canvas": {"width":1920,"height":1080,"frameRateNumerator":60,
                 "frameRateDenominator":1,"colorSpace":"rec709-sdr"},
      "database": "project.db",
      "directories": {"media":"media","audio":"audio","telemetry":"telemetry",
                      "proxies":"proxies","thumbnails":"thumbnails",
                      "autosave":"autosave","renders":"renders","logs":"logs"},
      "requiredFeatures": []
    })JSON");
}

TEST(ManifestSchemaValidatorTest, AcceptsCommittedManifestShape) {
    EXPECT_TRUE(validateManifestJson(validManifest()).hasValue());
}

TEST(ManifestSchemaValidatorTest, RejectsAdditionalRootProperty) {
    auto json = validManifest();
    json["unexpected"] = true;

    const auto result = validateManifestJson(json);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::ParseFailure);
}

TEST(ManifestSchemaValidatorTest, RejectsInvalidUuidAndDateTimeFormats) {
    const std::array<std::pair<std::string, std::string>, 2> mutations{{
        {"projectId", "not-a-uuid"},
        {"createdAt", "yesterday"},
    }};

    for (const auto& [field, value] : mutations) {
        auto json = validManifest();
        json[field] = value;

        const auto result = validateManifestJson(json);

        ASSERT_FALSE(result.hasValue()) << field;
        EXPECT_EQ(result.error().code(), ErrorCode::ParseFailure) << field;
    }
}

TEST(ManifestSchemaValidatorTest, RejectsDuplicateRequiredFeatures) {
    auto json = validManifest();
    json["requiredFeatures"] = {"avatar-2d", "avatar-2d"};

    const auto result = validateManifestJson(json);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::ParseFailure);
}

}  // namespace
