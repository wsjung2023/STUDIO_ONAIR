#include "project_store/JsonProjectStore.h"

#include "core/Utc.h"
#include "core/Uuid.h"
#include "domain/Identifiers.h"
#include "domain/ProjectManifest.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

using creator::core::ErrorCode;
using creator::domain::ProjectManifest;
using creator::project_store::JsonProjectStore;

/// Creates a unique temp directory per test and removes it afterwards, so a
/// failing test cannot leave state that makes the next run pass or fail
/// differently.
class JsonProjectStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        packageDir_ = fs::temp_directory_path() /
                      ("cs_test_" + std::string{info->test_suite_name()} + "_" +
                       std::string{info->name()});
        std::error_code ec;
        fs::remove_all(packageDir_, ec);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(packageDir_, ec);
    }

    void writeManifestText(const std::string& text) {
        fs::create_directories(packageDir_);
        std::ofstream out{packageDir_ / JsonProjectStore::kManifestFileName, std::ios::binary};
        out << text;
    }

    fs::path packageDir_;
    JsonProjectStore store_;
};

TEST_F(JsonProjectStoreTest, CreateWritesManifestAndDirectories) {
    const auto created = store_.create(packageDir_, "MyTutorial");

    ASSERT_TRUE(created.hasValue()) << created.error().message();
    EXPECT_TRUE(fs::exists(packageDir_ / JsonProjectStore::kManifestFileName));
    EXPECT_TRUE(fs::is_directory(packageDir_ / "media"));
    EXPECT_TRUE(fs::is_directory(packageDir_ / "audio"));
    EXPECT_TRUE(fs::is_directory(packageDir_ / "telemetry"));
    EXPECT_TRUE(fs::is_directory(packageDir_ / "proxies"));
    EXPECT_TRUE(fs::is_directory(packageDir_ / "thumbnails"));
    EXPECT_TRUE(fs::is_directory(packageDir_ / "autosave"));
    EXPECT_TRUE(fs::is_directory(packageDir_ / "renders"));
    EXPECT_TRUE(fs::is_directory(packageDir_ / "logs"));
}

TEST_F(JsonProjectStoreTest, CreateGeneratesUuidProjectId) {
    const auto created = store_.create(packageDir_, "MyTutorial");

    ASSERT_TRUE(created.hasValue());
    EXPECT_TRUE(creator::core::isUuidV4(created.value().projectId.value()));
}

TEST_F(JsonProjectStoreTest, CreateProducesValidManifest) {
    const auto created = store_.create(packageDir_, "MyTutorial");

    ASSERT_TRUE(created.hasValue());
    EXPECT_TRUE(creator::domain::validate(created.value()).hasValue());
    EXPECT_EQ(created.value().name, "MyTutorial");
    EXPECT_EQ(created.value().schemaVersion, ProjectManifest::kCurrentSchemaVersion);
}

TEST_F(JsonProjectStoreTest, RoundTripsThroughDisk) {
    const auto created = store_.create(packageDir_, "MyTutorial");
    ASSERT_TRUE(created.hasValue());

    const auto loaded = store_.load(packageDir_);

    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
    EXPECT_EQ(loaded.value(), created.value());
}

TEST_F(JsonProjectStoreTest, RoundTripsNonDefaultCanvas) {
    auto created = store_.create(packageDir_, "Shorts");
    ASSERT_TRUE(created.hasValue());

    ProjectManifest manifest = created.value();
    manifest.canvas.width = 1080;
    manifest.canvas.height = 1920;
    manifest.canvas.frameRateNumerator = 60000;
    manifest.canvas.frameRateDenominator = 1001;
    manifest.requiredFeatures = {"avatar-2d", "cursor-zoom"};
    ASSERT_TRUE(store_.save(packageDir_, manifest).hasValue());

    const auto loaded = store_.load(packageDir_);

    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
    EXPECT_EQ(loaded.value(), manifest);
    EXPECT_EQ(loaded.value().canvas.frameRateNumerator, 60000);
    EXPECT_EQ(loaded.value().canvas.frameRateDenominator, 1001);
    EXPECT_EQ(loaded.value().requiredFeatures.size(), 2u);
}

TEST_F(JsonProjectStoreTest, RoundTripsUnicodeName) {
    auto created = store_.create(packageDir_, "회사 교육 영상");
    ASSERT_TRUE(created.hasValue());

    const auto loaded = store_.load(packageDir_);

    ASSERT_TRUE(loaded.hasValue());
    EXPECT_EQ(loaded.value().name, "회사 교육 영상");
}

TEST_F(JsonProjectStoreTest, RoundTripsThroughNonAsciiPath) {
    // The path, not the content. A package directory outside the process ANSI
    // codepage must still work - this is a Korean-first product and users will
    // have Korean, Japanese and emoji in their folder names.
    //
    // Built with the char8_t (u8"...") overload of fs::path's constructor, not
    // L"...". On Windows, fs::path's *native* representation is wchar_t, so a
    // wide literal happens to work there too - but the C++20 standard defines
    // char8_t sources as always UTF-8, converted to each platform's native
    // encoding without going through any codepage or locale. That is not
    // guaranteed for wchar_t on POSIX, where the wide-to-native-narrow
    // conversion is implementation-defined. char8_t is the one construction
    // that is portable by standard, not by platform coincidence - see the
    // report for why this matters for macOS, where CI runs.
    const fs::path unicodeDir = packageDir_ / fs::path{u8"프로젝트_日本語_🎬"};

    const auto created = store_.create(unicodeDir, "MyTutorial");

    ASSERT_TRUE(created.hasValue()) << created.error().message();
    const auto loaded = store_.load(unicodeDir);
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
    EXPECT_EQ(loaded.value(), created.value());

    std::error_code ec;
    fs::remove_all(unicodeDir, ec);
}

TEST_F(JsonProjectStoreTest, SaveIsAtomicAndLeavesNoPartFile) {
    const auto created = store_.create(packageDir_, "MyTutorial");
    ASSERT_TRUE(created.hasValue());

    // This used to pass with writeFileAtomically replaced by a plain
    // ofstream straight over the target: with no failure injected, there is
    // no .part file to find left behind either way, so the old version of
    // this test could not tell "atomic" from "not atomic". Pre-creating
    // manifest.json.part as a directory forces the write path to fail (a
    // directory cannot stand in for the regular file writeFileAtomically
    // expects), which is what actually exercises the property this class
    // promises: a failed write must not destroy the good file already there.
    const fs::path partFile =
        packageDir_ / (std::string{JsonProjectStore::kManifestFileName} + ".part");
    fs::create_directory(partFile);

    ProjectManifest updated = created.value();
    updated.name = "Renamed";
    const auto saved = store_.save(packageDir_, updated);

    ASSERT_FALSE(saved.hasValue());
    EXPECT_EQ(saved.error().code(), ErrorCode::IoFailure);

    // The original manifest must survive a failed save untouched.
    const auto loaded = store_.load(packageDir_);
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
    EXPECT_EQ(loaded.value().name, "MyTutorial");
}

TEST_F(JsonProjectStoreTest, CreateRefusesToOverwriteExistingProject) {
    ASSERT_TRUE(store_.create(packageDir_, "First").hasValue());

    const auto second = store_.create(packageDir_, "Second");

    ASSERT_FALSE(second.hasValue());
    EXPECT_EQ(second.error().code(), ErrorCode::AlreadyExists);

    // The original must survive: overwriting would discard a recording.
    const auto loaded = store_.load(packageDir_);
    ASSERT_TRUE(loaded.hasValue());
    EXPECT_EQ(loaded.value().name, "First");
}

TEST_F(JsonProjectStoreTest, CreateRejectsInvalidName) {
    const auto created = store_.create(packageDir_, "");

    ASSERT_FALSE(created.hasValue());
    EXPECT_EQ(created.error().code(), ErrorCode::InvalidArgument);
    // Nothing may be left behind by a rejected create - not the manifest, and
    // not the directory tree either. create()'s own comment claims the
    // validate-before-create_directories ordering exists for exactly this,
    // but moving serialiseManifest()/validate() back below create_directories
    // would still pass a check that only looked for manifest.json: it is
    // never written either way, since JSON serialisation only happens after
    // both checks. Only checking that a subdirectory was not created actually
    // exercises the ordering.
    EXPECT_FALSE(fs::exists(packageDir_ / JsonProjectStore::kManifestFileName));
    EXPECT_FALSE(fs::exists(packageDir_ / "media"));
}

TEST_F(JsonProjectStoreTest, RejectsMalformedUtf8Name) {
    // A lone continuation byte: valid as far as validate()'s lead-byte count is
    // concerned, but not valid UTF-8, and nlohmann's dump() throws on it.
    const std::string malformed = std::string{"bad"} + '\x80' + "name";

    const auto created = store_.create(packageDir_, malformed);

    ASSERT_FALSE(created.hasValue());
    EXPECT_EQ(created.error().code(), ErrorCode::InvalidArgument);
    // Nothing may be left behind by a rejected create. Checking manifest.json
    // alone is not enough: this name passes validate() (it only counts UTF-8
    // lead bytes for the length limit) and is rejected later, by
    // serialiseManifest()'s dump() - so if create_directories() ever moved
    // ahead of that check, this file would still never get written, but a
    // whole tree of empty media/audio/telemetry/... directories would be left
    // behind unnoticed.
    EXPECT_FALSE(fs::exists(packageDir_ / JsonProjectStore::kManifestFileName));
    EXPECT_FALSE(fs::exists(packageDir_ / "media"));
}

TEST_F(JsonProjectStoreTest, LoadReportsMissingManifest) {
    const auto loaded = store_.load(packageDir_);

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::NotFound);
}

TEST_F(JsonProjectStoreTest, LoadReportsCorruptedJson) {
    writeManifestText("{ this is not json");

    const auto loaded = store_.load(packageDir_);

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::ParseFailure);
}

TEST_F(JsonProjectStoreTest, LoadReportsTruncatedJson) {
    // What a crash mid-write looks like if atomic rename were not used.
    writeManifestText(R"({"schemaVersion":1,"projectId":"123e4567-e89b-42d3-a45)");

    const auto loaded = store_.load(packageDir_);

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::ParseFailure);
}

TEST_F(JsonProjectStoreTest, LoadReportsNonObjectRoot) {
    writeManifestText("[1, 2, 3]");

    const auto loaded = store_.load(packageDir_);

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::ParseFailure);
}

TEST_F(JsonProjectStoreTest, LoadReportsMissingRequiredField) {
    // Every required field from schemas/project.schema.json, one at a time.
    for (const char* field : {"schemaVersion", "projectId", "name", "createdAt", "updatedAt",
                              "canvas", "database", "directories"}) {
        const auto created = store_.create(packageDir_, "MyTutorial");
        ASSERT_TRUE(created.hasValue());

        std::ifstream in{packageDir_ / JsonProjectStore::kManifestFileName, std::ios::binary};
        std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        in.close();

        auto json = nlohmann::json::parse(text);
        json.erase(field);
        writeManifestText(json.dump());

        const auto loaded = store_.load(packageDir_);

        ASSERT_FALSE(loaded.hasValue()) << "should have rejected manifest without " << field;
        EXPECT_EQ(loaded.error().code(), ErrorCode::ParseFailure) << field;

        std::error_code ec;
        fs::remove_all(packageDir_, ec);
    }
}

TEST_F(JsonProjectStoreTest, LoadReportsWrongFieldType) {
    writeManifestText(R"({
        "schemaVersion": "one",
        "projectId": "123e4567-e89b-42d3-a456-426614174000",
        "name": "MyTutorial",
        "createdAt": "2026-07-16T09:30:00Z",
        "updatedAt": "2026-07-16T09:30:00Z",
        "canvas": {"width":1920,"height":1080,"frameRateNumerator":60,
                   "frameRateDenominator":1,"colorSpace":"rec709-sdr"},
        "database": "project.db",
        "directories": {"media":"media","audio":"audio","telemetry":"telemetry",
                        "proxies":"proxies","thumbnails":"thumbnails",
                        "autosave":"autosave","renders":"renders","logs":"logs"}
    })");

    const auto loaded = store_.load(packageDir_);

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::ParseFailure);
}

TEST_F(JsonProjectStoreTest, LoadReportsFutureSchemaVersion) {
    const auto created = store_.create(packageDir_, "MyTutorial");
    ASSERT_TRUE(created.hasValue());

    std::ifstream in{packageDir_ / JsonProjectStore::kManifestFileName, std::ios::binary};
    auto json = nlohmann::json::parse(in);
    in.close();
    json["schemaVersion"] = ProjectManifest::kCurrentSchemaVersion + 1;
    writeManifestText(json.dump());

    const auto loaded = store_.load(packageDir_);

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::UnsupportedVersion);
}

TEST_F(JsonProjectStoreTest, FutureSchemaVersionTakesPrecedenceOverUnknownProperties) {
    const auto created = store_.create(packageDir_, "MyTutorial");
    ASSERT_TRUE(created.hasValue());

    std::ifstream in{packageDir_ / JsonProjectStore::kManifestFileName, std::ios::binary};
    auto json = nlohmann::json::parse(in);
    in.close();
    json["schemaVersion"] = ProjectManifest::kCurrentSchemaVersion + 1;
    json["futureProperty"] = true;
    writeManifestText(json.dump());

    const auto loaded = store_.load(packageDir_);

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::UnsupportedVersion);
}

TEST_F(JsonProjectStoreTest, LoadReportsUnknownColorSpace) {
    const auto created = store_.create(packageDir_, "MyTutorial");
    ASSERT_TRUE(created.hasValue());

    std::ifstream in{packageDir_ / JsonProjectStore::kManifestFileName, std::ios::binary};
    auto json = nlohmann::json::parse(in);
    in.close();
    json["canvas"]["colorSpace"] = "rec2020-hdr";
    writeManifestText(json.dump());

    const auto loaded = store_.load(packageDir_);

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::ParseFailure);
}

TEST_F(JsonProjectStoreTest, LoadReportsInvalidUuid) {
    const auto created = store_.create(packageDir_, "MyTutorial");
    ASSERT_TRUE(created.hasValue());

    std::ifstream in{packageDir_ / JsonProjectStore::kManifestFileName, std::ios::binary};
    auto json = nlohmann::json::parse(in);
    in.close();
    json["projectId"] = "not-a-uuid";
    writeManifestText(json.dump());

    const auto loaded = store_.load(packageDir_);

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::ParseFailure);
}

TEST_F(JsonProjectStoreTest, SaveRejectsInvalidManifest) {
    auto created = store_.create(packageDir_, "MyTutorial");
    ASSERT_TRUE(created.hasValue());

    ProjectManifest manifest = created.value();
    manifest.name = "";

    const auto saved = store_.save(packageDir_, manifest);

    ASSERT_FALSE(saved.hasValue());
    EXPECT_EQ(saved.error().code(), ErrorCode::InvalidArgument);

    // The invalid manifest must not have reached disk.
    const auto loaded = store_.load(packageDir_);
    ASSERT_TRUE(loaded.hasValue());
    EXPECT_EQ(loaded.value().name, "MyTutorial");
}

TEST_F(JsonProjectStoreTest, SaveReportsUnwritableLocation) {
    const auto created = store_.create(packageDir_, "MyTutorial");
    ASSERT_TRUE(created.hasValue());

    const auto saved = store_.save(packageDir_ / "no" / "such" / "place", created.value());

    ASSERT_FALSE(saved.hasValue());
    EXPECT_EQ(saved.error().code(), ErrorCode::IoFailure);
}

TEST_F(JsonProjectStoreTest, WrittenJsonMatchesSchemaFieldNames) {
    ASSERT_TRUE(store_.create(packageDir_, "MyTutorial").hasValue());

    std::ifstream in{packageDir_ / JsonProjectStore::kManifestFileName, std::ios::binary};
    const auto json = nlohmann::json::parse(in);

    // Field names must match schemas/project.schema.json exactly, or a manifest
    // this build writes will not validate against the shipped schema.
    for (const char* field : {"schemaVersion", "projectId", "name", "createdAt", "updatedAt",
                              "canvas", "database", "directories", "requiredFeatures"}) {
        EXPECT_TRUE(json.contains(field)) << "missing " << field;
    }
    EXPECT_TRUE(json["canvas"].contains("frameRateNumerator"));
    EXPECT_TRUE(json["canvas"].contains("frameRateDenominator"));
    EXPECT_TRUE(json["canvas"].contains("colorSpace"));
    EXPECT_EQ(json["database"], "project.db");
    // additionalProperties is false in the schema: an extra key we invent here
    // makes every manifest we write fail validation in R0-02.
    EXPECT_EQ(json.size(), 9u);
}

}  // namespace
