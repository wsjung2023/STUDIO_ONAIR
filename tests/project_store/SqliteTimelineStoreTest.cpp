#include "project_store/SqliteTimelineStore.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "core/Utc.h"
#include "domain/EditCommand.h"
#include "domain/Identifiers.h"
#include "domain/MediaAsset.h"
#include "domain/ProjectManifest.h"
#include "domain/Timeline.h"
#include "domain/TimelineTypes.h"
#include "project_store/ITimelineStore.h"
#include "project_store/SqliteProjectDatabase.h"
#include "project_store/internal/SqliteConnection.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace {

namespace fs = std::filesystem;

using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::FrameRate;
using creator::core::TimestampNs;
using creator::core::Utc;
using creator::domain::AssetAvailability;
using creator::domain::AssetId;
using creator::domain::AudioAssetMetadata;
using creator::domain::AudioEnvelope;
using creator::domain::Clip;
using creator::domain::ClipId;
using creator::domain::CommandId;
using creator::domain::EditCommandRecord;
using creator::domain::MediaAsset;
using creator::domain::MediaKind;
using creator::domain::ProjectId;
using creator::domain::ProjectManifest;
using creator::domain::TimeRange;
using creator::domain::Timeline;
using creator::domain::TimelineId;
using creator::domain::Track;
using creator::domain::TrackId;
using creator::domain::TrackKind;
using creator::domain::VideoAssetMetadata;
using creator::domain::VisualTransform;
using creator::project_store::EditEventKind;
using creator::project_store::EditEventRecord;
using creator::project_store::SqliteProjectDatabase;
using creator::project_store::SqliteTimelineStore;
using creator::project_store::TimelineCommit;
using creator::project_store::internal::SqliteConnection;

Utc utc(std::string_view text) {
    return Utc::parseRfc3339(text).value();
}

ProjectId projectId() {
    return ProjectId::create("123e4567-e89b-42d3-a456-426614174000").value();
}

ProjectManifest manifest() {
    return ProjectManifest{
        .schemaVersion = ProjectManifest::kCurrentSchemaVersion,
        .projectId = projectId(),
        .name = "R1 Timeline",
        .createdAt = utc("2026-07-17T00:00:00Z"),
        .updatedAt = utc("2026-07-17T00:00:00Z"),
        .canvas = {},
        .database = std::string{ProjectManifest::kDatabaseFileName},
        .directories = {},
        .requiredFeatures = {},
    };
}

TimestampNs at(std::int64_t value) {
    return TimestampNs{DurationNs{value}};
}

MediaAsset videoAsset(std::string fingerprint = "video-hash") {
    return MediaAsset::create(
               AssetId::create("video-asset").value(), MediaKind::Video,
               "media/video.mkv", DurationNs{1000},
               VideoAssetMetadata{.width = 3840,
                                  .height = 2160,
                                  .frameRate = FrameRate::create(60000, 1001).value()},
               std::nullopt, 10'000, std::move(fingerprint),
               AssetAvailability::Available)
        .value();
}

MediaAsset audioAsset() {
    return MediaAsset::create(
               AssetId::create("audio-asset").value(), MediaKind::Audio,
               "audio/mic.mka", DurationNs{1000}, std::nullopt,
               AudioAssetMetadata{.sampleRate = 48'000, .channels = 2},
               5'000, "audio-hash", AssetAvailability::Offline)
        .value();
}

Timeline populatedTimeline() {
    auto timeline = Timeline::create(TimelineId::create("main").value(), "Main",
                                     FrameRate::create(60000, 1001).value())
                        .value();
    const auto videoTrack = TrackId::create("v1").value();
    const auto audioTrack = TrackId::create("a1").value();
    EXPECT_TRUE(timeline.addTrack(
                            Track::create(videoTrack, TrackKind::Video, "Screen", true, false)
                                .value())
                    .hasValue());
    EXPECT_TRUE(timeline.addTrack(
                            Track::create(audioTrack, TrackKind::Audio, "Microphone", true, false)
                                .value())
                    .hasValue());
    EXPECT_TRUE(timeline.insertClip(
                            videoTrack,
                            Clip::createAsset(
                                ClipId::create("video-clip").value(), videoAsset(),
                                TimeRange::create(at(100), DurationNs{400}).value(),
                                TimeRange::create(at(0), DurationNs{400}).value(), true,
                                VisualTransform::create(
                                    0.1, 0.2, 0.7, 0.6, 1.1, 0.9, 5.0,
                                    0.01, 0.02, 0.03, 0.04, 0.8, 2)
                                    .value(),
                                std::nullopt)
                                .value())
                    .hasValue());
    EXPECT_TRUE(timeline.insertClip(
                            audioTrack,
                            Clip::createAsset(
                                ClipId::create("audio-clip").value(), audioAsset(),
                                TimeRange::create(at(200), DurationNs{300}).value(),
                                TimeRange::create(at(50), DurationNs{300}).value(), true,
                                std::nullopt,
                                AudioEnvelope::create(-4.5, DurationNs{20},
                                                      DurationNs{30}, DurationNs{300})
                                    .value())
                                .value())
                    .hasValue());
    return timeline;
}

EditEventRecord event(std::string eventId = "event-1") {
    return EditEventRecord{
        .eventId = std::move(eventId),
        .kind = EditEventKind::Apply,
        .command = EditCommandRecord{
            .commandId = CommandId::create("command-1").value(),
            .type = "TEST_EDIT",
            .payload = "{\"value\":1}",
            .undoPayload = "{\"value\":0}"},
        .createdAt = utc("2026-07-17T00:00:01Z")};
}

class SqliteTimelineStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ = fs::temp_directory_path() /
                     ("cs_timeline_store_" + std::string{info->name()});
        std::error_code ec;
        fs::remove_all(directory_, ec);
        fs::create_directories(directory_);
        ASSERT_TRUE(SqliteProjectDatabase::create(databasePath(), manifest()).hasValue());
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(directory_, ec);
    }

    fs::path databasePath() const { return directory_ / "project.db"; }

    SqliteTimelineStore store() {
        return SqliteTimelineStore::open(databasePath(), projectId()).value();
    }

    fs::path directory_;
};

TEST_F(SqliteTimelineStoreTest, StoresAssetsIdempotentlyAndRejectsIdentityConflict) {
    auto timelineStore = store();
    ASSERT_TRUE(timelineStore.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(timelineStore.putAsset(audioAsset()).hasValue());
    ASSERT_TRUE(timelineStore.putAsset(videoAsset()).hasValue());

    const auto loadedVideo = timelineStore.asset(AssetId::create("video-asset").value());
    ASSERT_TRUE(loadedVideo.hasValue());
    EXPECT_EQ(loadedVideo.value(), videoAsset());
    const auto all = timelineStore.assets();
    ASSERT_TRUE(all.hasValue());
    ASSERT_EQ(all.value().size(), 2U);

    const auto conflict = timelineStore.putAsset(videoAsset("different-hash"));
    ASSERT_FALSE(conflict.hasValue());
    EXPECT_EQ(conflict.error().code(), ErrorCode::AlreadyExists);
}

TEST_F(SqliteTimelineStoreTest, RoundTripsMultitrackSnapshotAcrossReopen) {
    const auto expected = populatedTimeline();
    {
        auto timelineStore = store();
        ASSERT_TRUE(timelineStore.putAsset(videoAsset()).hasValue());
        ASSERT_TRUE(timelineStore.putAsset(audioAsset()).hasValue());
        ASSERT_TRUE(timelineStore.createTimeline(expected).hasValue());
    }

    auto reopened = store();
    const auto loaded = reopened.loadPrimaryTimeline();

    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
    EXPECT_EQ(loaded.value().timeline, expected);
    EXPECT_EQ(loaded.value().revision, 0);
    EXPECT_EQ(loaded.value().historyCount, 0U);
    EXPECT_EQ(loaded.value().historyCursor, 0U);
    ASSERT_TRUE(loaded.value().cleanCursor.has_value());
    EXPECT_EQ(*loaded.value().cleanCursor, 0U);
    EXPECT_TRUE(loaded.value().events.empty());
}

TEST_F(SqliteTimelineStoreTest, CommitsSnapshotEventAndCursorAtomically) {
    auto timelineStore = store();
    ASSERT_TRUE(timelineStore.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(timelineStore.putAsset(audioAsset()).hasValue());
    const auto snapshot = populatedTimeline();
    ASSERT_TRUE(timelineStore.createTimeline(snapshot).hasValue());

    ASSERT_TRUE(timelineStore.commitEdit(
                                 TimelineCommit{.snapshot = snapshot,
                                                .expectedRevision = 0,
                                                .event = event(),
                                                .historyCount = 1,
                                                .historyCursor = 1,
                                                .cleanCursor = std::size_t{0}})
                    .hasValue());

    const auto loaded = timelineStore.loadPrimaryTimeline();
    ASSERT_TRUE(loaded.hasValue());
    EXPECT_EQ(loaded.value().revision, 1);
    EXPECT_EQ(loaded.value().historyCount, 1U);
    EXPECT_EQ(loaded.value().historyCursor, 1U);
    ASSERT_EQ(loaded.value().events.size(), 1U);
    EXPECT_EQ(loaded.value().events[0], event());
}

TEST_F(SqliteTimelineStoreTest, StaleRevisionLeavesStoredStateUnchanged) {
    auto timelineStore = store();
    ASSERT_TRUE(timelineStore.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(timelineStore.putAsset(audioAsset()).hasValue());
    const auto snapshot = populatedTimeline();
    ASSERT_TRUE(timelineStore.createTimeline(snapshot).hasValue());

    const auto result = timelineStore.commitEdit(
        TimelineCommit{.snapshot = snapshot,
                       .expectedRevision = 7,
                       .event = event(),
                       .historyCount = 1,
                       .historyCursor = 1,
                       .cleanCursor = std::size_t{0}});

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
    const auto loaded = timelineStore.loadPrimaryTimeline();
    ASSERT_TRUE(loaded.hasValue());
    EXPECT_EQ(loaded.value().revision, 0);
    EXPECT_TRUE(loaded.value().events.empty());
}

TEST_F(SqliteTimelineStoreTest, ConstraintFailureRollsBackSnapshotAndRevision) {
    auto timelineStore = store();
    ASSERT_TRUE(timelineStore.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(timelineStore.putAsset(audioAsset()).hasValue());
    const auto snapshot = populatedTimeline();
    ASSERT_TRUE(timelineStore.createTimeline(snapshot).hasValue());
    ASSERT_TRUE(timelineStore.commitEdit(
                                 TimelineCommit{.snapshot = snapshot,
                                                .expectedRevision = 0,
                                                .event = event("same-event"),
                                                .historyCount = 1,
                                                .historyCursor = 1,
                                                .cleanCursor = std::size_t{0}})
                    .hasValue());

    const auto duplicate = timelineStore.commitEdit(
        TimelineCommit{.snapshot = snapshot,
                       .expectedRevision = 1,
                       .event = event("same-event"),
                       .historyCount = 2,
                       .historyCursor = 2,
                       .cleanCursor = std::size_t{0}});

    ASSERT_FALSE(duplicate.hasValue());
    const auto loaded = timelineStore.loadPrimaryTimeline();
    ASSERT_TRUE(loaded.hasValue());
    EXPECT_EQ(loaded.value().revision, 1);
    EXPECT_EQ(loaded.value().historyCount, 1U);
    EXPECT_EQ(loaded.value().events.size(), 1U);
}

TEST_F(SqliteTimelineStoreTest, RejectsProjectIdentityMismatch) {
    const auto result = SqliteTimelineStore::open(
        databasePath(), ProjectId::create("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa").value());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(SqliteTimelineStoreTest, RejectsMalformedPersistedTrackKind) {
    auto timelineStore = store();
    ASSERT_TRUE(timelineStore.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(timelineStore.putAsset(audioAsset()).hasValue());
    ASSERT_TRUE(timelineStore.createTimeline(populatedTimeline()).hasValue());
    auto rawResult = SqliteConnection::open(databasePath());
    ASSERT_TRUE(rawResult.hasValue());
    auto raw = std::move(rawResult).value();
    ASSERT_TRUE(raw.execute(
        "PRAGMA ignore_check_constraints=ON; UPDATE tracks SET kind='BROKEN' WHERE track_id='v1';")
                    .hasValue());

    const auto loaded = timelineStore.loadPrimaryTimeline();

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::IoFailure);
}

}  // namespace
