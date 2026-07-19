#include "project_store/SqliteTimelineStore.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "core/Utc.h"
#include "domain/EditCommand.h"
#include "domain/DeleteRangeCommand.h"
#include "domain/Identifiers.h"
#include "domain/ImportRecordingCommand.h"
#include "domain/MediaAsset.h"
#include "domain/ProjectManifest.h"
#include "domain/SplitClipCommand.h"
#include "domain/SetVisualTransformCommand.h"
#include "domain/Timeline.h"
#include "domain/TimelineTypes.h"
#include "project_store/ITimelineStore.h"
#include "project_store/SqliteProjectDatabase.h"
#include "project_store/internal/SqliteConnection.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
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
using creator::domain::CaptionCue;
using creator::domain::CommandId;
using creator::domain::CueId;
using creator::domain::DeleteRangeCommand;
using creator::domain::EditCommandRecord;
using creator::domain::ImportRecordingCommand;
using creator::domain::MarkerId;
using creator::domain::MediaAsset;
using creator::domain::MediaKind;
using creator::domain::ProjectId;
using creator::domain::ProjectManifest;
using creator::domain::RgbaColor;
using creator::domain::SplitClipCommand;
using creator::domain::SetVisualTransformCommand;
using creator::domain::TimeRange;
using creator::domain::TextAlignment;
using creator::domain::Timeline;
using creator::domain::TimelineId;
using creator::domain::TimelineMarker;
using creator::domain::Track;
using creator::domain::TrackId;
using creator::domain::TrackKind;
using creator::domain::TitlePayload;
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

Timeline timelineWithMarkers() {
    auto timeline = populatedTimeline();
    EXPECT_TRUE(timeline.addMarker(TimelineMarker::create(
        MarkerId::create("start").value(), at(0), "녹화 시작").value()).hasValue());
    EXPECT_TRUE(timeline.addMarker(TimelineMarker::create(
        MarkerId::create("chapter").value(), at(250), "챕터 1").value()).hasValue());
    return timeline;
}

Timeline generatedTimeline() {
    auto timeline = populatedTimeline();
    const auto titleTrack = TrackId::create("title-1").value();
    const auto captionTrack = TrackId::create("caption-1").value();
    EXPECT_TRUE(timeline.addTrack(
        Track::create(titleTrack, TrackKind::Title,
                      "제목", true, false).value()).hasValue());
    EXPECT_TRUE(timeline.addTrack(
        Track::create(captionTrack, TrackKind::Caption,
                      "자막", true, false).value()).hasValue());
    const auto foreground = RgbaColor::parse("#ffffffff").value();
    const auto background = RgbaColor::parse("#00000080").value();
    EXPECT_TRUE(timeline.insertClip(
        titleTrack, Clip::createTitle(
            ClipId::create("title-clip").value(),
            TimeRange::create(at(100), DurationNs{300}).value(), true,
            TitlePayload::create("한글 제목", "Malgun Gothic", 0.5, 0.9,
                                 foreground, background,
                                 TextAlignment::Center).value(),
            VisualTransform::create(
                0.1, 0.1, 0.8, 0.8, 1.0, 1.0, 0.0,
                0.0, 0.0, 0.0, 0.0, 0.9, 5).value()).value()).hasValue());
    EXPECT_TRUE(timeline.insertClip(
        captionTrack, Clip::createCaption(
            ClipId::create("caption-clip").value(),
            TimeRange::create(at(200), DurationNs{400}).value(), true,
            {CaptionCue::create(CueId::create("cue-1").value(),
                                DurationNs{0}, DurationNs{100},
                                "첫 번째 자막").value(),
             CaptionCue::create(CueId::create("cue-2").value(),
                                DurationNs{150}, DurationNs{200},
                                "두 번째 자막").value()},
            std::nullopt).value()).hasValue());
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

TEST_F(SqliteTimelineStoreTest, ReportsPackagePathCollisionAsIdentityConflict) {
    auto timelineStore = store();
    ASSERT_TRUE(timelineStore.putAsset(videoAsset()).hasValue());
    auto colliding = MediaAsset::create(
        AssetId::create("other-video-asset").value(), MediaKind::Video,
        "media/video.mkv", DurationNs{1000},
        VideoAssetMetadata{.width = 3840,
                           .height = 2160,
                           .frameRate = FrameRate::create(60000, 1001).value()},
        std::nullopt, 10'000, "other-video-hash",
        AssetAvailability::Available);
    ASSERT_TRUE(colliding.hasValue());

    const auto result = timelineStore.putAsset(colliding.value());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::AlreadyExists);
}

TEST_F(SqliteTimelineStoreTest, ConcurrentIdenticalAssetInsertionsAreIdempotent) {
    auto firstResult = SqliteTimelineStore::open(databasePath(), projectId());
    auto secondResult = SqliteTimelineStore::open(databasePath(), projectId());
    ASSERT_TRUE(firstResult.hasValue());
    ASSERT_TRUE(secondResult.hasValue());
    auto first = std::move(firstResult).value();
    auto second = std::move(secondResult).value();
    std::barrier ready{3};
    std::atomic_bool firstOk{false};
    std::atomic_bool secondOk{false};
    std::thread firstThread{
        [store = std::move(first), &ready, &firstOk]() mutable {
            ready.arrive_and_wait();
            firstOk = store.putAsset(videoAsset()).hasValue();
        }};
    std::thread secondThread{
        [store = std::move(second), &ready, &secondOk]() mutable {
            ready.arrive_and_wait();
            secondOk = store.putAsset(videoAsset()).hasValue();
        }};
    ready.arrive_and_wait();
    firstThread.join();
    secondThread.join();
    EXPECT_TRUE(firstOk);
    EXPECT_TRUE(secondOk);

    auto verified = store();
    auto all = verified.assets();
    ASSERT_TRUE(all.hasValue());
    ASSERT_EQ(all.value().size(), 1U);
    EXPECT_EQ(all.value().front(), videoAsset());
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

TEST_F(SqliteTimelineStoreTest, RoundTripsTimelineMarkersAcrossReopen) {
    const auto expected = timelineWithMarkers();
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
    ASSERT_EQ(loaded.value().timeline.markers().size(), 2U);
    EXPECT_EQ(loaded.value().timeline.markers()[1].label(), "챕터 1");
}

TEST_F(SqliteTimelineStoreTest, RestoresImportRecordingHistoryAcrossReopen) {
    auto base = Timeline::create(TimelineId::create("main").value(), "Main",
                                 FrameRate::create(60000, 1001).value())
                    .value();
    const auto imported = timelineWithMarkers();
    auto created = ImportRecordingCommand::create(
        CommandId::create("import-recording").value(), imported.tracks(),
        imported.markers());
    ASSERT_TRUE(created.hasValue());
    auto command = std::move(created).value();
    auto snapshot = base;
    ASSERT_TRUE(command->execute(snapshot).hasValue());
    const auto record = command->record();

    {
        auto timelineStore = store();
        ASSERT_TRUE(timelineStore.putAsset(videoAsset()).hasValue());
        ASSERT_TRUE(timelineStore.putAsset(audioAsset()).hasValue());
        ASSERT_TRUE(timelineStore.createTimeline(base).hasValue());
        ASSERT_TRUE(timelineStore.commitEdit(TimelineCommit{
            .snapshot = snapshot,
            .expectedRevision = 0,
            .event = EditEventRecord{
                .eventId = "import-recording-event",
                .kind = EditEventKind::Apply,
                .command = record,
                .createdAt = utc("2026-07-17T00:00:01Z")},
            .historyCount = 1,
            .historyCursor = 1,
            .cleanCursor = std::size_t{0}}).hasValue());
    }

    auto reopened = store();
    auto session = reopened.loadEditSession(100);

    ASSERT_TRUE(session.hasValue()) << session.error().message();
    EXPECT_EQ(session.value().persisted.timeline, snapshot);
    ASSERT_TRUE(session.value().history.undo(
        session.value().persisted.timeline).hasValue());
    EXPECT_EQ(session.value().persisted.timeline, base);
    ASSERT_TRUE(session.value().history.redo(
        session.value().persisted.timeline).hasValue());
    EXPECT_EQ(session.value().persisted.timeline, snapshot);
}

TEST_F(SqliteTimelineStoreTest, RejectsCorruptImportRecordingUndoPayload) {
    auto base = Timeline::create(TimelineId::create("main").value(), "Main",
                                 FrameRate::create(60000, 1001).value())
                    .value();
    const auto imported = timelineWithMarkers();
    auto created = ImportRecordingCommand::create(
        CommandId::create("import-recording-corrupt").value(),
        imported.tracks(), imported.markers());
    ASSERT_TRUE(created.hasValue());
    auto command = std::move(created).value();
    auto snapshot = base;
    ASSERT_TRUE(command->execute(snapshot).hasValue());

    auto timelineStore = store();
    ASSERT_TRUE(timelineStore.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(timelineStore.putAsset(audioAsset()).hasValue());
    ASSERT_TRUE(timelineStore.createTimeline(base).hasValue());
    ASSERT_TRUE(timelineStore.commitEdit(TimelineCommit{
        .snapshot = snapshot,
        .expectedRevision = 0,
        .event = EditEventRecord{
            .eventId = "corrupt-import-event",
            .kind = EditEventKind::Apply,
            .command = command->record(),
            .createdAt = utc("2026-07-17T00:00:01Z")},
        .historyCount = 1,
        .historyCursor = 1,
        .cleanCursor = std::size_t{0}}).hasValue());
    auto rawResult = SqliteConnection::open(databasePath());
    ASSERT_TRUE(rawResult.hasValue());
    auto raw = std::move(rawResult).value();
    ASSERT_TRUE(raw.execute(
        "UPDATE edit_commands SET undo_payload_json="
        "'{\"unexpected\":true}' WHERE event_id='corrupt-import-event';")
                    .hasValue());

    const auto history = timelineStore.loadEditHistory(100);

    ASSERT_FALSE(history.hasValue());
    EXPECT_EQ(history.error().code(), ErrorCode::ParseFailure);
}

TEST_F(SqliteTimelineStoreTest, RejectsConflictingPersistedMarkerPosition) {
    auto timelineStore = store();
    ASSERT_TRUE(timelineStore.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(timelineStore.putAsset(audioAsset()).hasValue());
    ASSERT_TRUE(timelineStore.createTimeline(timelineWithMarkers()).hasValue());
    auto rawResult = SqliteConnection::open(databasePath());
    ASSERT_TRUE(rawResult.hasValue());
    auto raw = std::move(rawResult).value();
    ASSERT_TRUE(raw.execute(
        "INSERT INTO markers(marker_id,timeline_id,position_ns,label) "
        "VALUES('conflict','main',0,'conflict');").hasValue());

    const auto loaded = timelineStore.loadPrimaryTimeline();

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::IoFailure);
}

TEST_F(SqliteTimelineStoreTest, RejectsRealPersistedMarkerPosition) {
    auto timelineStore = store();
    ASSERT_TRUE(timelineStore.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(timelineStore.putAsset(audioAsset()).hasValue());
    ASSERT_TRUE(timelineStore.createTimeline(timelineWithMarkers()).hasValue());
    auto rawResult = SqliteConnection::open(databasePath());
    ASSERT_TRUE(rawResult.hasValue());
    auto raw = std::move(rawResult).value();
    ASSERT_TRUE(raw.execute(
        "UPDATE markers SET position_ns=1.5 WHERE marker_id='chapter';")
                    .hasValue());

    const auto loaded = timelineStore.loadPrimaryTimeline();

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::IoFailure);
}

TEST_F(SqliteTimelineStoreTest, RoundTripsGeneratedUnicodeClipsAcrossReopen) {
    const auto expected = generatedTimeline();
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
}

TEST_F(SqliteTimelineStoreTest, RejectsMissingGeneratedPayloadAcrossReopen) {
    auto timelineStore = store();
    ASSERT_TRUE(timelineStore.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(timelineStore.putAsset(audioAsset()).hasValue());
    ASSERT_TRUE(timelineStore.createTimeline(generatedTimeline()).hasValue());
    auto rawResult = SqliteConnection::open(databasePath());
    ASSERT_TRUE(rawResult.hasValue());
    auto raw = std::move(rawResult).value();
    ASSERT_TRUE(raw.execute(
        "DELETE FROM titles WHERE clip_id='title-clip';").hasValue());

    const auto loaded = timelineStore.loadPrimaryTimeline();

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::IoFailure);
}

TEST_F(SqliteTimelineStoreTest, RejectsOverlappingPersistedCaptionCues) {
    auto timelineStore = store();
    ASSERT_TRUE(timelineStore.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(timelineStore.putAsset(audioAsset()).hasValue());
    ASSERT_TRUE(timelineStore.createTimeline(generatedTimeline()).hasValue());
    auto rawResult = SqliteConnection::open(databasePath());
    ASSERT_TRUE(rawResult.hasValue());
    auto raw = std::move(rawResult).value();
    ASSERT_TRUE(raw.execute(
        "UPDATE caption_cues SET start_offset_ns=50 WHERE cue_id='cue-2';")
                    .hasValue());

    const auto loaded = timelineStore.loadPrimaryTimeline();

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::IoFailure);
}

TEST_F(SqliteTimelineStoreTest, RejectsFutureEffectCommandVersion) {
    auto timelineStore = store();
    ASSERT_TRUE(timelineStore.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(timelineStore.putAsset(audioAsset()).hasValue());
    auto snapshot = populatedTimeline();
    ASSERT_TRUE(timelineStore.createTimeline(snapshot).hasValue());
    SetVisualTransformCommand command{
        CommandId::create("future-effect").value(), TrackId::create("v1").value(),
        ClipId::create("video-clip").value(),
        VisualTransform::create(
            0.2, 0.2, 0.5, 0.5, 1.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 0.0, 1.0, 3).value()};
    ASSERT_TRUE(command.execute(snapshot).hasValue());
    auto record = command.record();
    const auto version = record.payload.find("\"version\":1");
    ASSERT_NE(version, std::string::npos);
    record.payload.replace(version, std::string{"\"version\":1"}.size(),
                           "\"version\":2");
    ASSERT_TRUE(timelineStore.commitEdit(
        TimelineCommit{
            .snapshot = snapshot,
            .expectedRevision = 0,
            .event = EditEventRecord{
                .eventId = "future-effect-event",
                .kind = EditEventKind::Apply,
                .command = std::move(record),
                .createdAt = utc("2026-07-17T00:00:01Z")},
            .historyCount = 1,
            .historyCursor = 1,
            .cleanCursor = std::size_t{0}}).hasValue());

    const auto history = timelineStore.loadEditHistory(100);

    ASSERT_FALSE(history.hasValue());
    EXPECT_EQ(history.error().code(), ErrorCode::ParseFailure);
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

TEST_F(SqliteTimelineStoreTest,
       RejectsAtomicAssetsWithoutImportMetadataBeforeMutation) {
    auto timelineStore = store();
    const auto snapshot = Timeline::create(
                              TimelineId::create("main").value(), "Main",
                              FrameRate::create(60, 1).value())
                              .value();
    ASSERT_TRUE(timelineStore.createTimeline(snapshot).hasValue());

    const auto result = timelineStore.commitEdit(
        TimelineCommit{.snapshot = snapshot,
                       .expectedRevision = 0,
                       .event = event("asset-without-import"),
                       .historyCount = 1,
                       .historyCursor = 1,
                       .cleanCursor = std::size_t{0},
                       .assetsToInsert = {videoAsset()}});

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    const auto loaded = timelineStore.loadPrimaryTimeline();
    const auto assets = timelineStore.assets();
    ASSERT_TRUE(loaded.hasValue());
    ASSERT_TRUE(assets.hasValue());
    EXPECT_EQ(loaded.value().revision, 0);
    EXPECT_TRUE(loaded.value().events.empty());
    EXPECT_TRUE(assets.value().empty());
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

TEST_F(SqliteTimelineStoreTest, RejectsPersistedIntegersOutsideDomainRanges) {
    auto timelineStore = store();
    ASSERT_TRUE(timelineStore.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(timelineStore.putAsset(audioAsset()).hasValue());
    ASSERT_TRUE(timelineStore.createTimeline(populatedTimeline()).hasValue());
    auto rawResult = SqliteConnection::open(databasePath());
    ASSERT_TRUE(rawResult.hasValue());
    auto raw = std::move(rawResult).value();
    ASSERT_TRUE(raw.execute(
        "PRAGMA ignore_check_constraints=ON; "
        "UPDATE media_assets SET width=-2147483649 WHERE asset_id='video-asset';")
                    .hasValue());

    const auto invalidAsset = timelineStore.asset(
        AssetId::create("video-asset").value());

    ASSERT_FALSE(invalidAsset.hasValue());
    EXPECT_EQ(invalidAsset.error().code(), ErrorCode::IoFailure);
}

TEST_F(SqliteTimelineStoreTest, RejectsPersistedCheckpointOutsideTimelineState) {
    auto timelineStore = store();
    ASSERT_TRUE(timelineStore.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(timelineStore.putAsset(audioAsset()).hasValue());
    ASSERT_TRUE(timelineStore.createTimeline(populatedTimeline()).hasValue());
    auto rawResult = SqliteConnection::open(databasePath());
    ASSERT_TRUE(rawResult.hasValue());
    auto raw = std::move(rawResult).value();
    ASSERT_TRUE(raw.execute(
        "PRAGMA ignore_check_constraints=ON; "
        "UPDATE edit_checkpoints SET clean_cursor=1,explicit_saved_revision=1;")
                    .hasValue());

    const auto loaded = timelineStore.loadPrimaryTimeline();

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::IoFailure);
}

TEST_F(SqliteTimelineStoreTest, RejectsUnsignedOverflowInPersistedCommandJson) {
    auto timelineStore = store();
    ASSERT_TRUE(timelineStore.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(timelineStore.putAsset(audioAsset()).hasValue());
    auto snapshot = populatedTimeline();
    ASSERT_TRUE(timelineStore.createTimeline(snapshot).hasValue());
    SplitClipCommand command{
        CommandId::create("overflow-command").value(),
        TrackId::create("v1").value(), ClipId::create("video-clip").value(),
        ClipId::create("video-right").value(), at(200)};
    ASSERT_TRUE(command.execute(snapshot).hasValue());
    auto record = command.record();
    record.payload =
        "{\"clipId\":\"video-clip\",\"rightClipId\":\"video-right\","
        "\"splitNs\":18446744073709551615,\"trackId\":\"v1\"}";
    ASSERT_TRUE(timelineStore.commitEdit(
                                 TimelineCommit{
                                     .snapshot = snapshot,
                                     .expectedRevision = 0,
                                     .event = EditEventRecord{
                                         .eventId = "overflow-event",
                                         .kind = EditEventKind::Apply,
                                         .command = std::move(record),
                                         .createdAt = utc("2026-07-17T00:00:01Z")},
                                     .historyCount = 1,
                                     .historyCursor = 1,
                                     .cleanCursor = std::size_t{0}})
                    .hasValue());

    const auto history = timelineStore.loadEditHistory(100);
    ASSERT_FALSE(history.hasValue());
    EXPECT_EQ(history.error().code(), ErrorCode::ParseFailure);
}

TEST_F(SqliteTimelineStoreTest, RejectsValidButInconsistentCommandUndoState) {
    auto timelineStore = store();
    ASSERT_TRUE(timelineStore.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(timelineStore.putAsset(audioAsset()).hasValue());
    auto snapshot = populatedTimeline();
    ASSERT_TRUE(timelineStore.createTimeline(snapshot).hasValue());
    SplitClipCommand command{
        CommandId::create("tampered-undo-command").value(),
        TrackId::create("v1").value(), ClipId::create("video-clip").value(),
        ClipId::create("video-right").value(), at(200)};
    ASSERT_TRUE(command.execute(snapshot).hasValue());
    auto record = command.record();
    const auto sourceStart = record.undoPayload.find("\"sourceStartNs\":100");
    ASSERT_NE(sourceStart, std::string::npos);
    record.undoPayload.replace(sourceStart, std::string{"\"sourceStartNs\":100"}.size(),
                               "\"sourceStartNs\":101");
    ASSERT_TRUE(timelineStore.commitEdit(
                                 TimelineCommit{
                                     .snapshot = snapshot,
                                     .expectedRevision = 0,
                                     .event = EditEventRecord{
                                         .eventId = "tampered-undo-event",
                                         .kind = EditEventKind::Apply,
                                         .command = std::move(record),
                                         .createdAt = utc("2026-07-17T00:00:01Z")},
                                     .historyCount = 1,
                                     .historyCursor = 1,
                                     .cleanCursor = std::size_t{0}})
                    .hasValue());

    const auto history = timelineStore.loadEditHistory(100);

    ASSERT_FALSE(history.hasValue());
    EXPECT_EQ(history.error().code(), ErrorCode::IoFailure);
}

TEST_F(SqliteTimelineStoreTest, RejectsEventSequenceGapAgainstRevision) {
    auto timelineStore = store();
    ASSERT_TRUE(timelineStore.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(timelineStore.putAsset(audioAsset()).hasValue());
    const auto snapshot = populatedTimeline();
    ASSERT_TRUE(timelineStore.createTimeline(snapshot).hasValue());
    ASSERT_TRUE(timelineStore.commitEdit(
                                 TimelineCommit{.snapshot = snapshot,
                                                .expectedRevision = 0,
                                                .event = event("sequence-event"),
                                                .historyCount = 1,
                                                .historyCursor = 1,
                                                .cleanCursor = std::size_t{0}})
                    .hasValue());
    auto rawResult = SqliteConnection::open(databasePath());
    ASSERT_TRUE(rawResult.hasValue());
    auto raw = std::move(rawResult).value();
    ASSERT_TRUE(raw.execute(
        "UPDATE edit_commands SET sequence=2 WHERE event_id='sequence-event';")
                    .hasValue());

    const auto loaded = timelineStore.loadPrimaryTimeline();

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::IoFailure);
}

TEST_F(SqliteTimelineStoreTest, RejectsDuplicateGeneratedIdsInDeleteCommandJson) {
    auto timelineStore = store();
    ASSERT_TRUE(timelineStore.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(timelineStore.putAsset(audioAsset()).hasValue());
    auto snapshot = populatedTimeline();
    ASSERT_TRUE(timelineStore.createTimeline(snapshot).hasValue());
    DeleteRangeCommand command{
        CommandId::create("delete-duplicates").value(),
        TimeRange::create(at(100), DurationNs{100}).value(), true,
        {ClipId::create("video-right").value(),
         ClipId::create("audio-right").value()}};
    ASSERT_TRUE(command.execute(snapshot).hasValue());
    auto record = command.record();
    record.payload =
        "{\"durationNs\":100,\"rightClipIds\":[\"same-right\",\"same-right\"],"
        "\"ripple\":true,\"startNs\":100}";
    ASSERT_TRUE(timelineStore.commitEdit(
                                 TimelineCommit{
                                     .snapshot = snapshot,
                                     .expectedRevision = 0,
                                     .event = EditEventRecord{
                                         .eventId = "duplicate-delete-event",
                                         .kind = EditEventKind::Apply,
                                         .command = std::move(record),
                                         .createdAt = utc("2026-07-17T00:00:01Z")},
                                     .historyCount = 1,
                                     .historyCursor = 1,
                                     .cleanCursor = std::size_t{0}})
                    .hasValue());

    const auto history = timelineStore.loadEditHistory(100);

    ASSERT_FALSE(history.hasValue());
    EXPECT_EQ(history.error().code(), ErrorCode::ParseFailure);
}

}  // namespace
