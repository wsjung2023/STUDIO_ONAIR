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

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <winioctl.h>
#endif

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

#ifdef _WIN32
std::error_code createDirectoryJunction(const fs::path& junction,
                                        const fs::path& target) {
    struct MountPointReparseData final {
        DWORD tag;
        WORD dataLength;
        WORD reserved;
        WORD substituteNameOffset;
        WORD substituteNameLength;
        WORD printNameOffset;
        WORD printNameLength;
        WCHAR pathBuffer[1];
    };

    if (!CreateDirectoryW(junction.c_str(), nullptr)) {
        return {static_cast<int>(GetLastError()), std::system_category()};
    }
    const std::wstring printName = fs::absolute(target).wstring();
    const std::wstring substituteName = L"\\??\\" + printName;
    const auto substituteBytes = static_cast<WORD>(substituteName.size() * sizeof(WCHAR));
    const auto printBytes = static_cast<WORD>(printName.size() * sizeof(WCHAR));
    constexpr std::size_t mountPointHeaderBytes = 8;
    const std::size_t pathBytes = static_cast<std::size_t>(substituteBytes) +
                                  sizeof(WCHAR) + printBytes + sizeof(WCHAR);
    alignas(void*) std::array<std::byte, MAXIMUM_REPARSE_DATA_BUFFER_SIZE> storage{};
    auto* data = reinterpret_cast<MountPointReparseData*>(storage.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->dataLength = static_cast<WORD>(mountPointHeaderBytes + pathBytes);
    data->substituteNameOffset = 0;
    data->substituteNameLength = substituteBytes;
    data->printNameOffset = static_cast<WORD>(substituteBytes + sizeof(WCHAR));
    data->printNameLength = printBytes;
    std::memcpy(data->pathBuffer, substituteName.data(), substituteBytes);
    std::memcpy(reinterpret_cast<std::byte*>(data->pathBuffer) + data->printNameOffset,
                printName.data(), printBytes);

    const HANDLE handle = CreateFileW(
        junction.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = std::error_code{static_cast<int>(GetLastError()),
                                           std::system_category()};
        RemoveDirectoryW(junction.c_str());
        return error;
    }
    DWORD returned = 0;
    const DWORD inputBytes = static_cast<DWORD>(8 + data->dataLength);
    const BOOL created = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, data,
                                         inputBytes, nullptr, 0, &returned, nullptr);
    const DWORD code = created ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!created) RemoveDirectoryW(junction.c_str());
    return {static_cast<int>(code), std::system_category()};
}
#endif

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

    void writeBytes(const fs::path& path, std::string_view bytes) {
        fs::create_directories(path.parent_path());
        std::ofstream out{path, std::ios::binary};
        out << bytes;
        ASSERT_TRUE(out.good());
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

TEST_F(ProjectPackageStoreTest, OpenDoesNotCreateAMissingDatabase) {
    ASSERT_TRUE(store_.create(packagePath_, "Missing DB").hasValue());
    ASSERT_TRUE(fs::remove(packagePath_ / "project.db"));

    const auto result = store_.open(packagePath_);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    EXPECT_FALSE(fs::exists(packagePath_ / "project.db"));
}

TEST_F(ProjectPackageStoreTest, OpenRejectsDatabaseHardLinkedFromOutsidePackage) {
    ASSERT_TRUE(store_.create(packagePath_, "Hard Link").hasValue());
    const fs::path outsideDatabase = root_ / "outside.db";
    ASSERT_TRUE(fs::copy_file(packagePath_ / "project.db", outsideDatabase));
    ASSERT_TRUE(fs::remove(packagePath_ / "project.db"));
    std::error_code ec;
    fs::create_hard_link(outsideDatabase, packagePath_ / "project.db", ec);
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_GT(fs::hard_link_count(outsideDatabase), 1u);

    const auto result = store_.open(packagePath_);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(ProjectPackageStoreTest, OpenRejectsDatabaseReparseFromOutsidePackage) {
    ASSERT_TRUE(store_.create(packagePath_, "Reparse").hasValue());
    ASSERT_TRUE(fs::remove(packagePath_ / "project.db"));
#ifdef _WIN32
    const fs::path outsideDirectory = root_ / "outside-database";
    ASSERT_TRUE(fs::create_directory(outsideDirectory));
    const auto junctionError =
        createDirectoryJunction(packagePath_ / "project.db", outsideDirectory);
    ASSERT_FALSE(junctionError) << junctionError.message();
#else
    const fs::path outsideDatabase = root_ / "outside.db";
    std::ofstream{outsideDatabase, std::ios::binary} << "outside";
    std::error_code ec;
    fs::create_symlink(outsideDatabase, packagePath_ / "project.db", ec);
    ASSERT_FALSE(ec) << ec.message();
#endif

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

TEST_F(ProjectPackageStoreTest, RecoverQuarantinesKnownAndOrphanPartFiles) {
    ASSERT_TRUE(store_.create(packagePath_, "Recovery files").hasValue());
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
        .relativePath = "media/screen/screen-1/segment_000000.mkv",
    };
    ASSERT_TRUE(store_.beginSegment(packagePath_, session, writing).hasValue());
    const fs::path knownSource =
        packagePath_ / ".tmp" / fs::path{writing.relativePath + ".part"};
    const fs::path orphanSource = packagePath_ / ".tmp/loose/orphan.mkv.part";
    writeBytes(knownSource, "known-part");
    writeBytes(orphanSource, "orphan-part");

    const auto recovered =
        store_.recover(packagePath_, session, utc("2026-07-16T11:00:00Z"));

    ASSERT_TRUE(recovered.hasValue()) << recovered.error().message();
    EXPECT_EQ(recovered.value().quarantinedParts, 1u);
    EXPECT_EQ(recovered.value().orphanParts, 1u);
    EXPECT_FALSE(fs::exists(knownSource));
    EXPECT_FALSE(fs::exists(orphanSource));
    EXPECT_TRUE(fs::is_regular_file(
        packagePath_ / "recovery/quarantine/session-1/media/screen/screen-1/segment_000000.mkv.part"));
    EXPECT_TRUE(fs::is_regular_file(
        packagePath_ / "recovery/quarantine/orphans/loose/orphan.mkv.part"));

    const auto again =
        store_.recover(packagePath_, session, utc("2026-07-16T11:05:00Z"));
    ASSERT_TRUE(again.hasValue());
    EXPECT_EQ(again.value().quarantinedParts, 0u);
    EXPECT_EQ(again.value().orphanParts, 0u);
}

TEST_F(ProjectPackageStoreTest, RecoverNeverOverwritesExistingQuarantineFile) {
    ASSERT_TRUE(store_.create(packagePath_, "No overwrite").hasValue());
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
        .relativePath = "media/screen/screen-1/segment_000000.mkv",
    };
    ASSERT_TRUE(store_.beginSegment(packagePath_, session, writing).hasValue());
    const fs::path source =
        packagePath_ / ".tmp" / fs::path{writing.relativePath + ".part"};
    const fs::path destination = packagePath_ /
        "recovery/quarantine/session-1/media/screen/screen-1/segment_000000.mkv.part";
    writeBytes(source, "new-bytes");
    writeBytes(destination, "keep-bytes");

    const auto recovered =
        store_.recover(packagePath_, session, utc("2026-07-16T11:00:00Z"));

    ASSERT_FALSE(recovered.hasValue());
    EXPECT_EQ(recovered.error().code(), ErrorCode::AlreadyExists);
    EXPECT_TRUE(fs::is_regular_file(source));
    std::ifstream existing{destination, std::ios::binary};
    const std::string existingBytes{std::istreambuf_iterator<char>{existing},
                                    std::istreambuf_iterator<char>{}};
    EXPECT_EQ(existingBytes, "keep-bytes");
    EXPECT_EQ(store_.open(packagePath_).value().recoveryCandidates.size(), 1u);
}

TEST_F(ProjectPackageStoreTest, RecoverDoesNotTreatAnotherSessionPartAsOrphan) {
    ASSERT_TRUE(store_.create(packagePath_, "Two recoveries").hasValue());
    const auto first = SessionId::create("session-1").value();
    const auto second = SessionId::create("session-2").value();
    const auto source = SourceId::create("screen-1").value();
    ASSERT_TRUE(store_.beginRecording(packagePath_, first, TimestampNs{},
                                      utc("2026-07-16T10:00:00Z"))
                    .hasValue());
    ASSERT_TRUE(store_.beginRecording(packagePath_, second, TimestampNs{},
                                      utc("2026-07-16T10:01:00Z"))
                    .hasValue());
    const SegmentInfo firstWriting{
        .index = 0, .sourceId = source, .startTime = TimestampNs{},
        .duration = std::chrono::seconds{2}, .status = SegmentStatus::Writing,
        .relativePath = "media/screen/screen-1/first.mkv"};
    const SegmentInfo secondWriting{
        .index = 0, .sourceId = source, .startTime = TimestampNs{},
        .duration = std::chrono::seconds{2}, .status = SegmentStatus::Writing,
        .relativePath = "media/screen/screen-1/second.mkv"};
    ASSERT_TRUE(store_.beginSegment(packagePath_, first, firstWriting).hasValue());
    ASSERT_TRUE(store_.beginSegment(packagePath_, second, secondWriting).hasValue());
    const fs::path secondPart =
        packagePath_ / ".tmp" / fs::path{secondWriting.relativePath + ".part"};
    writeBytes(secondPart, "second-session");

    const auto recovered =
        store_.recover(packagePath_, first, utc("2026-07-16T11:00:00Z"));

    ASSERT_TRUE(recovered.hasValue()) << recovered.error().message();
    EXPECT_EQ(recovered.value().orphanParts, 0u);
    EXPECT_TRUE(fs::is_regular_file(secondPart));
}

TEST_F(ProjectPackageStoreTest, RecoverRejectsHardLinkedPartWithoutChangingDatabase) {
    ASSERT_TRUE(store_.create(packagePath_, "Unsafe part").hasValue());
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
        .relativePath = "media/screen/screen-1/segment_000000.mkv",
    };
    ASSERT_TRUE(store_.beginSegment(packagePath_, session, writing).hasValue());
    const fs::path outside = root_ / "outside.part";
    writeBytes(outside, "shared-bytes");
    const fs::path source =
        packagePath_ / ".tmp" / fs::path{writing.relativePath + ".part"};
    fs::create_directories(source.parent_path());
    std::error_code error;
    fs::create_hard_link(outside, source, error);
    if (error) GTEST_SKIP() << "hard link creation is unavailable: " << error.message();

    const auto recovered =
        store_.recover(packagePath_, session, utc("2026-07-16T11:00:00Z"));

    ASSERT_FALSE(recovered.hasValue());
    EXPECT_EQ(recovered.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(store_.open(packagePath_).value().recoveryCandidates.size(), 1u);
    EXPECT_EQ(fs::hard_link_count(outside), 2u);
}

TEST_F(ProjectPackageStoreTest, RecoverUnknownSessionDoesNotMoveOrphanParts) {
    ASSERT_TRUE(store_.create(packagePath_, "Unknown recovery").hasValue());
    const fs::path orphan = packagePath_ / ".tmp/loose/orphan.mkv.part";
    writeBytes(orphan, "keep-until-valid-recovery");

    const auto recovered = store_.recover(
        packagePath_, SessionId::create("unknown-session").value(),
        utc("2026-07-16T11:00:00Z"));

    ASSERT_FALSE(recovered.hasValue());
    EXPECT_EQ(recovered.error().code(), ErrorCode::NotFound);
    EXPECT_TRUE(fs::is_regular_file(orphan));
    EXPECT_FALSE(fs::exists(packagePath_ / "recovery/quarantine/orphans"));
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
