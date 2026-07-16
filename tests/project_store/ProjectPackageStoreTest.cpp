#include "project_store/ProjectPackageStore.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "core/Utc.h"
#include "domain/Identifiers.h"
#include "domain/RecordingSession.h"
#include "domain/Segment.h"
#include "project_store/SqliteProjectDatabase.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

using creator::core::ErrorCode;
using creator::core::TimestampNs;
using creator::core::Utc;
using creator::domain::SegmentInfo;
using creator::domain::SegmentStatus;
using creator::domain::RecordingSession;
using creator::domain::SessionId;
using creator::domain::SourceId;
using creator::project_store::ProjectPackageStore;
using creator::project_store::PersistedSessionState;
using creator::project_store::SqliteProjectDatabase;

Utc utc(std::string_view text) {
    return Utc::parseRfc3339(text).value();
}

class ProjectPackageStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        root_ = fs::temp_directory_path() /
                ("cs_package_" + std::string{info->test_suite_name()} + "_" +
                 std::string{info->name()});
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_);
        packagePath_ = root_ / "lesson.cstudio";
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    std::vector<fs::path> creatingEntries() const {
        std::vector<fs::path> result;
        std::error_code ec;
        for (fs::directory_iterator it{root_, ec}; !ec && it != fs::directory_iterator{};
             it.increment(ec)) {
            if (it->path().filename().string().find(".creating-") != std::string::npos) {
                result.push_back(it->path());
            }
        }
        return result;
    }

    void replaceManifestProjectId(std::string_view id) {
        const fs::path path = packagePath_ / "manifest.json";
        std::ifstream in{path, std::ios::binary};
        auto json = nlohmann::json::parse(in);
        in.close();
        json["projectId"] = id;
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        out << json.dump(2);
    }

    fs::path root_;
    fs::path packagePath_;
    ProjectPackageStore store_;
};

TEST_F(ProjectPackageStoreTest, CreatePublishesCompletePackageAtOnce) {
    const auto result = store_.create(packagePath_, "강의 프로젝트");

    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_EQ(result.value().package.path, packagePath_);
    EXPECT_EQ(result.value().package.manifest.name, "강의 프로젝트");
    EXPECT_TRUE(fs::exists(packagePath_ / "manifest.json"));
    EXPECT_TRUE(fs::exists(packagePath_ / "project.db"));
    EXPECT_TRUE(fs::is_directory(packagePath_ / "media"));
    EXPECT_TRUE(store_.open(packagePath_).hasValue());
    EXPECT_TRUE(creatingEntries().empty());
}

TEST_F(ProjectPackageStoreTest, CreateNeverOverwritesExistingTarget) {
    fs::create_directories(packagePath_);
    std::ofstream{packagePath_ / "keep.txt"} << "keep";

    const auto result = store_.create(packagePath_, "Second");

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::AlreadyExists);
    EXPECT_TRUE(fs::exists(packagePath_ / "keep.txt"));
    EXPECT_TRUE(creatingEntries().empty());
}

TEST_F(ProjectPackageStoreTest, OpenRejectsManifestDatabaseMismatch) {
    ASSERT_TRUE(store_.create(packagePath_, "Original").hasValue());
    replaceManifestProjectId("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");

    const auto result = store_.open(packagePath_);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(ProjectPackageStoreTest, CreateFailureRemovesOnlyGeneratedStagingSibling) {
    const fs::path regularParent = root_ / "not-a-directory";
    std::ofstream{regularParent} << "keep";
    const fs::path impossibleTarget = regularParent / "lesson.cstudio";

    const auto result = store_.create(impossibleTarget, "Cannot Create");

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::IoFailure);
    EXPECT_FALSE(fs::exists(impossibleTarget));
    EXPECT_TRUE(fs::is_regular_file(regularParent));
    EXPECT_TRUE(creatingEntries().empty());
}

TEST_F(ProjectPackageStoreTest, OpenReportsInterruptedRecordingThroughValidatedPackage) {
    ASSERT_TRUE(store_.create(packagePath_, "Recovery").hasValue());
    const auto session = SessionId::create("session-1").value();
    ASSERT_TRUE(store_.beginRecording(packagePath_, session, TimestampNs{},
                                      utc("2026-07-16T10:00:00Z"))
                    .hasValue());
    const SegmentInfo writing{
        .index = 0,
        .sourceId = SourceId::create("screen-1").value(),
        .startTime = TimestampNs{},
        .duration = std::chrono::seconds{2},
        .status = SegmentStatus::Writing,
        .relativePath = "media/screen-1/segment_0.mkv",
    };
    ASSERT_TRUE(store_.beginSegment(packagePath_, session, writing).hasValue());

    const auto opened = store_.open(packagePath_);

    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    ASSERT_EQ(opened.value().recoveryCandidates.size(), 1u);
    EXPECT_EQ(opened.value().recoveryCandidates[0].writingSegments, 1u);
}

TEST_F(ProjectPackageStoreTest, CompleteRecordingPersistsExactNonZeroStopTime) {
    const auto created = store_.create(packagePath_, "Complete");
    ASSERT_TRUE(created.hasValue());
    const auto sessionId = SessionId::create("session-complete").value();
    const auto startedAt = TimestampNs{} + std::chrono::seconds{5};
    const auto stoppedAt = TimestampNs{} + std::chrono::seconds{8};
    ASSERT_TRUE(store_.beginRecording(packagePath_, sessionId, startedAt,
                                      utc("2026-07-16T10:00:00Z"))
                    .hasValue());
    RecordingSession session{sessionId};
    ASSERT_TRUE(session.start(startedAt).hasValue());
    ASSERT_TRUE(session.stop(stoppedAt).hasValue());

    ASSERT_TRUE(store_.completeRecording(packagePath_, session,
                                         utc("2026-07-16T10:00:03Z"))
                    .hasValue());

    auto database = SqliteProjectDatabase::open(
        packagePath_ / "project.db", created.value().package.manifest.projectId);
    ASSERT_TRUE(database.hasValue());
    auto record = database.value().session(sessionId);
    ASSERT_TRUE(record.hasValue());
    EXPECT_EQ(record.value().state, PersistedSessionState::Completed);
    EXPECT_EQ(record.value().stoppedAt, stoppedAt);
}

}  // namespace
