#include "app/TimelineEditService.h"

#include "core/Timebase.h"
#include "core/Utc.h"
#include "domain/MediaAsset.h"
#include "domain/ProjectManifest.h"
#include "domain/SplitClipCommand.h"
#include "domain/TrimClipCommand.h"
#include "project_store/SqliteProjectDatabase.h"
#include "project_store/SqliteTimelineStore.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace {

namespace fs = std::filesystem;

using creator::app::TimelineEditService;
using creator::core::DurationNs;
using creator::core::FrameRate;
using creator::core::TimestampNs;
using creator::core::Utc;
using creator::domain::AssetAvailability;
using creator::domain::AssetId;
using creator::domain::Clip;
using creator::domain::ClipId;
using creator::domain::CommandId;
using creator::domain::MediaAsset;
using creator::domain::MediaKind;
using creator::domain::ProjectId;
using creator::domain::ProjectManifest;
using creator::domain::SplitClipCommand;
using creator::domain::TimeRange;
using creator::domain::Timeline;
using creator::domain::TimelineId;
using creator::domain::Track;
using creator::domain::TrackId;
using creator::domain::TrackKind;
using creator::domain::TrimClipCommand;
using creator::domain::TrimEdge;
using creator::domain::VideoAssetMetadata;
using creator::project_store::SqliteProjectDatabase;
using creator::project_store::SqliteTimelineStore;

Utc fixedUtc() {
    return Utc::parseRfc3339("2026-07-17T12:00:00Z").value();
}

ProjectId projectId() {
    return ProjectId::create("123e4567-e89b-42d3-a456-426614174100").value();
}

ProjectManifest manifest() {
    return ProjectManifest{
        .schemaVersion = ProjectManifest::kCurrentSchemaVersion,
        .projectId = projectId(),
        .name = "Durable timeline",
        .createdAt = fixedUtc(),
        .updatedAt = fixedUtc(),
        .canvas = {},
        .database = std::string{ProjectManifest::kDatabaseFileName},
        .directories = {},
        .requiredFeatures = {},
    };
}

TimestampNs at(std::int64_t value) {
    return TimestampNs{DurationNs{value}};
}

MediaAsset videoAsset() {
    return MediaAsset::create(
               AssetId::create("screen-asset").value(), MediaKind::Video,
               "media/screen.mkv", DurationNs{1'000},
               VideoAssetMetadata{.width = 1920,
                                  .height = 1080,
                                  .frameRate = FrameRate::create(60, 1).value()},
               std::nullopt, 42'000, "screen-fingerprint",
               AssetAvailability::Available)
        .value();
}

Timeline initialTimeline() {
    auto result = Timeline::create(TimelineId::create("main").value(), "Main",
                                   FrameRate::create(60, 1).value());
    EXPECT_TRUE(result.hasValue());
    auto timeline = std::move(result).value();
    const auto trackId = TrackId::create("screen-track").value();
    EXPECT_TRUE(timeline.addTrack(
                            Track::create(trackId, TrackKind::Video, "Screen", true,
                                          false)
                                .value())
                    .hasValue());
    EXPECT_TRUE(timeline.insertClip(
                            trackId,
                            Clip::createAsset(
                                ClipId::create("screen-clip").value(), videoAsset(),
                                TimeRange::create(at(100), DurationNs{600}).value(),
                                TimeRange::create(at(0), DurationNs{600}).value(), true,
                                std::nullopt, std::nullopt)
                                .value())
                    .hasValue());
    return timeline;
}

class TempProject final {
public:
    TempProject() {
        path_ = fs::temp_directory_path() /
                ("creator-studio-timeline-service-" +
                 std::to_string(++nextId_) + ".sqlite3");
        fs::remove(path_);
        EXPECT_TRUE(SqliteProjectDatabase::create(path_, manifest()).hasValue());
    }

    ~TempProject() { fs::remove(path_); }
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    inline static std::uint64_t nextId_{0};
    fs::path path_;
};

struct SequentialIds final {
    std::string operator()() { return "event-" + std::to_string(++next); }
    std::uint64_t next{0};
};

TEST(TimelineEditServiceTest, ReopensAtEveryCursorAndKeepsUndoRedoAndCleanState) {
    TempProject project;
    auto storeResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(storeResult.hasValue());
    auto store = std::move(storeResult).value();
    ASSERT_TRUE(store.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(store.createTimeline(initialTimeline()).hasValue());

    SequentialIds ids;
    auto serviceResult = TimelineEditService::open(
        store, 100, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(serviceResult.hasValue()) << serviceResult.error().message();
    auto service = std::move(serviceResult).value();
    EXPECT_EQ(service.revision(), 0);
    EXPECT_TRUE(service.isClean());

    ASSERT_TRUE(service.execute(std::make_unique<SplitClipCommand>(
                                    CommandId::create("split-1").value(),
                                    TrackId::create("screen-track").value(),
                                    ClipId::create("screen-clip").value(),
                                    ClipId::create("screen-right").value(), at(250)))
                    .hasValue());
    EXPECT_EQ(service.revision(), 1);
    EXPECT_FALSE(service.isClean());

    auto reopenedStoreResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(reopenedStoreResult.hasValue());
    auto reopenedStore = std::move(reopenedStoreResult).value();
    auto reopenedResult = TimelineEditService::open(
        reopenedStore, 100, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(reopenedResult.hasValue()) << reopenedResult.error().message();
    auto reopened = std::move(reopenedResult).value();
    EXPECT_EQ(reopened.snapshot(), service.snapshot());
    EXPECT_FALSE(reopened.isClean());

    ASSERT_TRUE(reopened.undo().hasValue());
    EXPECT_EQ(reopened.revision(), 2);
    EXPECT_EQ(reopened.snapshot(), initialTimeline());
    EXPECT_TRUE(reopened.isClean());

    auto undoneStoreResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(undoneStoreResult.hasValue());
    auto undoneStore = std::move(undoneStoreResult).value();
    auto undoneResult = TimelineEditService::open(
        undoneStore, 100, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(undoneResult.hasValue()) << undoneResult.error().message();
    auto undone = std::move(undoneResult).value();
    EXPECT_EQ(undone.snapshot(), initialTimeline());
    EXPECT_TRUE(undone.isClean());

    ASSERT_TRUE(undone.redo().hasValue());
    EXPECT_EQ(undone.revision(), 3);
    ASSERT_TRUE(undone.markExplicitSave().hasValue());
    EXPECT_TRUE(undone.isClean());

    auto savedStoreResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(savedStoreResult.hasValue());
    auto savedStore = std::move(savedStoreResult).value();
    auto savedResult = TimelineEditService::open(
        savedStore, 100, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(savedResult.hasValue()) << savedResult.error().message();
    EXPECT_EQ(savedResult.value().snapshot(), undone.snapshot());
    EXPECT_EQ(savedResult.value().revision(), 3);
    EXPECT_TRUE(savedResult.value().isClean());
}

TEST(TimelineEditServiceTest, FailedDurableCommitLeavesMemoryExactlyUnchanged) {
    TempProject project;
    auto storeResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(storeResult.hasValue());
    auto store = std::move(storeResult).value();
    ASSERT_TRUE(store.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(store.createTimeline(initialTimeline()).hasValue());

    auto serviceResult = TimelineEditService::open(
        store, 100, [] { return std::string{"duplicate-event"}; },
        [] { return fixedUtc(); });
    ASSERT_TRUE(serviceResult.hasValue());
    auto service = std::move(serviceResult).value();
    ASSERT_TRUE(service.execute(std::make_unique<SplitClipCommand>(
                                    CommandId::create("split-1").value(),
                                    TrackId::create("screen-track").value(),
                                    ClipId::create("screen-clip").value(),
                                    ClipId::create("screen-right").value(), at(250)))
                    .hasValue());

    const auto before = service.snapshot();
    const auto beforeRevision = service.revision();
    const auto failed = service.execute(std::make_unique<TrimClipCommand>(
        CommandId::create("trim-1").value(),
        TrackId::create("screen-track").value(),
        ClipId::create("screen-right").value(), TrimEdge::Trailing, at(500)));
    ASSERT_FALSE(failed.hasValue());
    EXPECT_EQ(service.snapshot(), before);
    EXPECT_EQ(service.revision(), beforeRevision);

    auto persisted = store.loadPrimaryTimeline();
    ASSERT_TRUE(persisted.hasValue());
    EXPECT_EQ(persisted.value().timeline, before);
    EXPECT_EQ(persisted.value().revision, beforeRevision);
}

TEST(TimelineEditServiceTest, NewEditAfterUndoPersistsWithoutTheOldRedoBranch) {
    TempProject project;
    auto storeResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(storeResult.hasValue());
    auto store = std::move(storeResult).value();
    ASSERT_TRUE(store.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(store.createTimeline(initialTimeline()).hasValue());

    SequentialIds ids;
    auto serviceResult = TimelineEditService::open(
        store, 100, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(serviceResult.hasValue());
    auto service = std::move(serviceResult).value();
    ASSERT_TRUE(service.execute(std::make_unique<SplitClipCommand>(
                                    CommandId::create("split-branch").value(),
                                    TrackId::create("screen-track").value(),
                                    ClipId::create("screen-clip").value(),
                                    ClipId::create("discarded-right").value(), at(250)))
                    .hasValue());
    ASSERT_TRUE(service.undo().hasValue());
    ASSERT_TRUE(service.execute(std::make_unique<TrimClipCommand>(
                                    CommandId::create("trim-branch").value(),
                                    TrackId::create("screen-track").value(),
                                    ClipId::create("screen-clip").value(),
                                    TrimEdge::Trailing, at(500)))
                    .hasValue());
    EXPECT_FALSE(service.redo().hasValue());
    const auto branched = service.snapshot();

    auto reopenedStoreResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(reopenedStoreResult.hasValue());
    auto reopenedStore = std::move(reopenedStoreResult).value();
    auto reopenedResult = TimelineEditService::open(
        reopenedStore, 100, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(reopenedResult.hasValue()) << reopenedResult.error().message();
    EXPECT_EQ(reopenedResult.value().snapshot(), branched);
    EXPECT_EQ(reopenedResult.value().revision(), 3);
    EXPECT_FALSE(reopenedResult.value().redo().hasValue());
}

}  // namespace
