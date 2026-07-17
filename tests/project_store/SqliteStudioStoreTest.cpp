#include "project_store/SqliteStudioStore.h"

#include "core/Utc.h"
#include "domain/StudioScene.h"
#include "project_store/MigrationRunner.h"
#include "project_store/internal/SqliteConnection.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <barrier>
#include <future>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

using creator::core::DurationNs;
using creator::core::TimestampNs;
using creator::domain::ProjectId;
using creator::domain::SceneId;
using creator::domain::SessionId;
using creator::domain::SourceId;
using creator::domain::StudioSourceRole;
using creator::domain::TimelineId;
using creator::domain::defaultStudioScenes;
using creator::project_store::RecordingImportRecord;
using creator::project_store::RecordingMarker;
using creator::project_store::RecordingSourceRole;
using creator::project_store::SqliteStudioStore;
using creator::project_store::StudioSnapshot;
using creator::project_store::internal::SqliteConnection;

class SqliteStudioStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ = fs::temp_directory_path() /
                     ("cs_studio_" + std::string{info->name()});
        std::error_code ec;
        fs::remove_all(directory_, ec);
        fs::create_directories(directory_);

        auto opened = SqliteConnection::open(databasePath());
        ASSERT_TRUE(opened.hasValue());
        auto connection = std::move(opened).value();
        ASSERT_TRUE(creator::project_store::MigrationRunner::apply(connection)
                        .hasValue());
        ASSERT_TRUE(connection.execute(
            "INSERT INTO projects VALUES("
            "'project','Studio',1,'2026-07-17T00:00:00Z',"
            "'2026-07-17T00:00:00Z');")
                        .hasValue());
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(directory_, ec);
    }

    fs::path databasePath() const { return directory_ / "project.db"; }

    ProjectId projectId() const { return ProjectId::create("project").value(); }
    SessionId sessionId() const { return SessionId::create("session").value(); }

    void insertRecordingSession(std::string_view state = "RECORDING") {
        auto opened = SqliteConnection::open(databasePath());
        ASSERT_TRUE(opened.hasValue());
        auto connection = std::move(opened).value();
        const std::string sql =
            "INSERT INTO recording_sessions VALUES("
            "'session','project','" + std::string{state} +
            "',1000," + (state == "RECORDING" ? std::string{"NULL"}
                                               : std::string{"5000"}) +
            ",'2026-07-17T00:00:00Z'," +
            (state == "RECORDING"
                 ? std::string{"NULL"}
                 : std::string{"'2026-07-17T00:00:04Z'"}) +
            ",NULL);";
        ASSERT_TRUE(connection.execute(sql).hasValue());
    }

    fs::path directory_;
};

TEST_F(SqliteStudioStoreTest, SeedsLoadsAndMutatesUnicodeScenesAtomically) {
    auto opened = SqliteStudioStore::open(databasePath(), projectId());
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto store = std::move(opened).value();
    const auto defaults = defaultStudioScenes();
    ASSERT_TRUE(defaults.hasValue());

    ASSERT_TRUE(store.seedDefaultsIfEmpty(defaults.value()).hasValue());
    ASSERT_TRUE(store.seedDefaultsIfEmpty(defaults.value()).hasValue());
    const auto before = store.load();
    ASSERT_TRUE(before.hasValue()) << before.error().message();
    ASSERT_EQ(before.value().scenes.size(), 3U);
    EXPECT_EQ(before.value().activeSceneId, before.value().scenes[0].id());

    auto renamed = before.value().scenes[0].withName("제품 강의");
    ASSERT_TRUE(renamed.hasValue());
    auto changedScenes = before.value().scenes;
    changedScenes[0] = std::move(renamed).value();
    const StudioSnapshot changed{.scenes = std::move(changedScenes),
                                 .activeSceneId = before.value().activeSceneId};
    ASSERT_TRUE(store.commitSceneMutation(changed).hasValue());

    const auto after = store.load();
    ASSERT_TRUE(after.hasValue());
    EXPECT_EQ(after.value().scenes[0].name(), "제품 강의");
    EXPECT_EQ(before.value().scenes[0].name(), "Presentation");
}

TEST_F(SqliteStudioStoreTest, PreparesRecordingAndPersistsEventsAndMarkers) {
    insertRecordingSession();
    auto opened = SqliteStudioStore::open(databasePath(), projectId());
    ASSERT_TRUE(opened.hasValue());
    auto store = std::move(opened).value();
    const auto defaults = defaultStudioScenes().value();
    ASSERT_TRUE(store.seedDefaultsIfEmpty(defaults).hasValue());

    const std::vector sources{
        RecordingSourceRole{.sourceId = SourceId::create("screen").value(),
                            .role = StudioSourceRole::Screen},
        RecordingSourceRole{.sourceId = SourceId::create("camera").value(),
                            .role = StudioSourceRole::Camera}};
    ASSERT_TRUE(store.prepareRecording(sessionId(), sources, defaults[0].id())
                    .hasValue());
    ASSERT_TRUE(store.recordSceneSwitch(
                         sessionId(), defaults[1].id(), 1,
                         TimestampNs{DurationNs{2'000'000'000}})
                    .hasValue());
    ASSERT_TRUE(store.recordMarker(RecordingMarker{
                         .markerId = "marker-1",
                         .sessionId = sessionId(),
                         .position = TimestampNs{DurationNs{3'000'000'000}},
                         .label = "중요"})
                    .hasValue());

    const auto events = store.loadRecordingSceneEvents(sessionId());
    ASSERT_TRUE(events.hasValue());
    ASSERT_EQ(events.value().size(), 2U);
    EXPECT_EQ(events.value()[0].sequence, 0U);
    EXPECT_EQ(events.value()[0].position.time_since_epoch(), DurationNs::zero());
    EXPECT_EQ(events.value()[1].sceneId, defaults[1].id());
    const auto markers = store.loadRecordingMarkers(sessionId());
    ASSERT_TRUE(markers.hasValue());
    ASSERT_EQ(markers.value().size(), 1U);
    EXPECT_EQ(markers.value()[0].label, "중요");
}

TEST_F(SqliteStudioStoreTest, RecordingSwitchFailurePublishesNothing) {
    insertRecordingSession();
    auto opened = SqliteStudioStore::open(databasePath(), projectId());
    ASSERT_TRUE(opened.hasValue());
    auto store = std::move(opened).value();
    const auto defaults = defaultStudioScenes().value();
    ASSERT_TRUE(store.seedDefaultsIfEmpty(defaults).hasValue());
    ASSERT_TRUE(store.prepareRecording(sessionId(), {}, defaults[0].id())
                    .hasValue());

    auto triggerConnection = SqliteConnection::open(databasePath());
    ASSERT_TRUE(triggerConnection.hasValue());
    ASSERT_TRUE(triggerConnection.value()
                    .execute(
                        "CREATE TRIGGER reject_scene_switch BEFORE INSERT ON "
                        "recording_scene_events WHEN NEW.sequence > 0 BEGIN "
                        "SELECT RAISE(ABORT, 'injected'); END;")
                    .hasValue());

    EXPECT_FALSE(store.recordSceneSwitch(
                          sessionId(), defaults[1].id(), 1,
                          TimestampNs{DurationNs{3'000'000'000}})
                     .hasValue());
    const auto events = store.loadRecordingSceneEvents(sessionId());
    ASSERT_TRUE(events.hasValue());
    ASSERT_EQ(events.value().size(), 1U);
    EXPECT_EQ(events.value()[0].sequence, 0U);
}

TEST_F(SqliteStudioStoreTest, SceneMutationPreservesCompletedRecordingEvents) {
    insertRecordingSession();
    auto opened = SqliteStudioStore::open(databasePath(), projectId());
    ASSERT_TRUE(opened.hasValue());
    auto store = std::move(opened).value();
    const auto defaults = defaultStudioScenes().value();
    ASSERT_TRUE(store.seedDefaultsIfEmpty(defaults).hasValue());
    ASSERT_TRUE(store.prepareRecording(sessionId(), {}, defaults[0].id())
                    .hasValue());
    {
        auto connection = SqliteConnection::open(databasePath());
        ASSERT_TRUE(connection.hasValue());
        ASSERT_TRUE(connection.value()
                        .execute(
                            "UPDATE recording_sessions SET state='COMPLETED',"
                            "stopped_ns=5000,finished_at_utc='2026-07-17T00:00:04Z' "
                            "WHERE session_id='session'")
                        .hasValue());
    }

    auto snapshot = store.load().value();
    snapshot.scenes[0] = snapshot.scenes[0].withName("완료 장면").value();
    ASSERT_TRUE(store.commitSceneMutation(snapshot).hasValue());

    const auto events = store.loadRecordingSceneEvents(sessionId());
    ASSERT_TRUE(events.hasValue());
    ASSERT_EQ(events.value().size(), 1U);
    EXPECT_EQ(events.value()[0].sceneId, defaults[0].id());
    EXPECT_EQ(store.load().value().scenes[0].name(), "완료 장면");
}

TEST_F(SqliteStudioStoreTest, FailedSceneMutationRollsBackWholeSnapshot) {
    auto opened = SqliteStudioStore::open(databasePath(), projectId());
    ASSERT_TRUE(opened.hasValue());
    auto store = std::move(opened).value();
    ASSERT_TRUE(store.seedDefaultsIfEmpty(defaultStudioScenes().value()).hasValue());
    const auto before = store.load().value();
    auto changed = before;
    changed.scenes[0] = changed.scenes[0].withName("실패해야 함").value();
    {
        auto connection = SqliteConnection::open(databasePath());
        ASSERT_TRUE(connection.hasValue());
        ASSERT_TRUE(connection.value()
                        .execute(
                            "CREATE TRIGGER reject_scene_name BEFORE UPDATE OF name ON "
                            "scenes BEGIN SELECT RAISE(ABORT, 'injected'); END;")
                        .hasValue());
    }

    EXPECT_FALSE(store.commitSceneMutation(changed).hasValue());
    const auto after = store.load();
    ASSERT_TRUE(after.hasValue());
    EXPECT_EQ(after.value(), before);
}

TEST_F(SqliteStudioStoreTest, ReordersScenesThroughOccupiedPositions) {
    auto opened = SqliteStudioStore::open(databasePath(), projectId());
    ASSERT_TRUE(opened.hasValue());
    auto store = std::move(opened).value();
    ASSERT_TRUE(store.seedDefaultsIfEmpty(defaultStudioScenes().value()).hasValue());
    auto snapshot = store.load().value();
    const auto firstId = snapshot.scenes[0].id();
    const auto secondId = snapshot.scenes[1].id();
    snapshot.scenes[0] = snapshot.scenes[0].withPosition(1).value();
    snapshot.scenes[1] = snapshot.scenes[1].withPosition(0).value();

    ASSERT_TRUE(store.commitSceneMutation(snapshot).hasValue());

    const auto reordered = store.load();
    ASSERT_TRUE(reordered.hasValue());
    EXPECT_EQ(reordered.value().scenes[0].id(), secondId);
    EXPECT_EQ(reordered.value().scenes[1].id(), firstId);
}

TEST_F(SqliteStudioStoreTest, CompletedRecordingIsListedUntilExactImportIsRecorded) {
    insertRecordingSession("COMPLETED");
    {
        auto connection = SqliteConnection::open(databasePath());
        ASSERT_TRUE(connection.hasValue());
        ASSERT_TRUE(connection.value()
                        .execute(
                            "INSERT INTO timelines(timeline_id,project_id,name,"
                            "frame_rate_numerator,frame_rate_denominator,revision,is_primary) "
                            "VALUES('timeline','project','Main',60,1,4,1)")
                        .hasValue());
    }
    auto opened = SqliteStudioStore::open(databasePath(), projectId());
    ASSERT_TRUE(opened.hasValue());
    auto store = std::move(opened).value();
    ASSERT_EQ(store.completedUnimportedRecordings().value().size(), 1U);
    const auto importedAt = creator::core::Utc::parseRfc3339(
        "2026-07-17T00:00:05Z").value();
    const RecordingImportRecord record{
        .sessionId = sessionId(),
        .timelineId = TimelineId::create("timeline").value(),
        .base = TimestampNs{DurationNs{8'000'000'000}},
        .importedRevision = 5,
        .importedAt = importedAt};

    ASSERT_TRUE(store.putRecordingImport(record).hasValue());
    ASSERT_TRUE(store.putRecordingImport(record).hasValue());
    EXPECT_TRUE(store.completedUnimportedRecordings().value().empty());
    ASSERT_TRUE(store.recordingImport(sessionId()).value().has_value());
    EXPECT_EQ(*store.recordingImport(sessionId()).value(), record);

    auto conflicting = record;
    conflicting.importedRevision = 6;
    EXPECT_FALSE(store.putRecordingImport(conflicting).hasValue());
}

TEST_F(SqliteStudioStoreTest, RejectsLiveWritesAfterRecordingCompletes) {
    insertRecordingSession("COMPLETED");
    auto opened = SqliteStudioStore::open(databasePath(), projectId());
    ASSERT_TRUE(opened.hasValue());
    auto store = std::move(opened).value();
    const auto defaults = defaultStudioScenes().value();
    ASSERT_TRUE(store.seedDefaultsIfEmpty(defaults).hasValue());

    EXPECT_FALSE(store.prepareRecording(sessionId(), {}, defaults[0].id())
                     .hasValue());
    EXPECT_FALSE(store.recordSceneSwitch(
                          sessionId(), defaults[0].id(), 0,
                          TimestampNs{DurationNs::zero()})
                     .hasValue());
    EXPECT_FALSE(store.recordMarker(RecordingMarker{
                          .markerId = "late",
                          .sessionId = sessionId(),
                          .position = TimestampNs{DurationNs{1}},
                          .label = "late"})
                     .hasValue());
}

TEST_F(SqliteStudioStoreTest, UnimportedRecordingLocksSceneSourceConfiguration) {
    insertRecordingSession();
    auto opened = SqliteStudioStore::open(databasePath(), projectId());
    ASSERT_TRUE(opened.hasValue());
    auto store = std::move(opened).value();
    const auto defaults = defaultStudioScenes().value();
    ASSERT_TRUE(store.seedDefaultsIfEmpty(defaults).hasValue());
    ASSERT_TRUE(store.prepareRecording(sessionId(), {}, defaults[0].id())
                    .hasValue());
    {
        auto connection = SqliteConnection::open(databasePath());
        ASSERT_TRUE(connection.hasValue());
        ASSERT_TRUE(connection.value()
                        .execute(
                            "UPDATE recording_sessions SET state='COMPLETED',"
                            "stopped_ns=5000,finished_at_utc='2026-07-17T00:00:04Z' "
                            "WHERE session_id='session'")
                        .hasValue());
    }
    const auto before = store.load().value();
    auto changed = before;
    auto camera = changed.scenes[0].sources()[1].withEnabled(false).value();
    changed.scenes[0] = changed.scenes[0].withSource(camera).value();

    EXPECT_FALSE(store.commitSceneMutation(changed).hasValue());
    EXPECT_EQ(store.load().value(), before);
}

TEST_F(SqliteStudioStoreTest, DiscardsAbortedRecordingPreparationIdempotently) {
    insertRecordingSession();
    auto opened = SqliteStudioStore::open(databasePath(), projectId());
    ASSERT_TRUE(opened.hasValue());
    auto store = std::move(opened).value();
    const auto defaults = defaultStudioScenes().value();
    ASSERT_TRUE(store.seedDefaultsIfEmpty(defaults).hasValue());
    ASSERT_TRUE(store.prepareRecording(
                         sessionId(),
                         {RecordingSourceRole{
                             .sourceId = SourceId::create("screen").value(),
                             .role = StudioSourceRole::Screen}},
                         defaults[0].id())
                    .hasValue());
    {
        auto connection = SqliteConnection::open(databasePath());
        ASSERT_TRUE(connection.hasValue());
        ASSERT_TRUE(connection.value()
                        .execute(
                            "UPDATE recording_sessions SET state='ABORTED',"
                            "stopped_ns=1000,finished_at_utc='2026-07-17T00:00:01Z',"
                            "failure_reason='engine start failed' "
                            "WHERE session_id='session'")
                        .hasValue());
    }

    ASSERT_TRUE(store.discardRecordingPreparation(sessionId()).hasValue());
    ASSERT_TRUE(store.discardRecordingPreparation(sessionId()).hasValue());
    EXPECT_TRUE(store.loadRecordingSources(sessionId()).value().empty());
    EXPECT_TRUE(store.loadRecordingSceneEvents(sessionId()).value().empty());
}

TEST_F(SqliteStudioStoreTest, RejectsPersistedZOrderOutsideInt32) {
    auto opened = SqliteStudioStore::open(databasePath(), projectId());
    ASSERT_TRUE(opened.hasValue());
    auto store = std::move(opened).value();
    ASSERT_TRUE(store.seedDefaultsIfEmpty(defaultStudioScenes().value()).hasValue());
    {
        auto connection = SqliteConnection::open(databasePath());
        ASSERT_TRUE(connection.hasValue());
        ASSERT_TRUE(connection.value().execute("PRAGMA ignore_check_constraints=ON")
                        .hasValue());
        ASSERT_TRUE(connection.value()
                        .execute(
                            "UPDATE scene_sources SET z_order=2147483648 "
                            "WHERE scene_id='presentation' AND source_id='screen'")
                        .hasValue());
    }

    EXPECT_FALSE(store.load().hasValue());
}

TEST_F(SqliteStudioStoreTest, RejectsImportCheckpointWhileSessionIsLive) {
    insertRecordingSession();
    {
        auto connection = SqliteConnection::open(databasePath());
        ASSERT_TRUE(connection.hasValue());
        ASSERT_TRUE(connection.value()
                        .execute(
                            "INSERT INTO timelines(timeline_id,project_id,name,"
                            "frame_rate_numerator,frame_rate_denominator,revision,is_primary) "
                            "VALUES('timeline','project','Main',60,1,0,1)")
                        .hasValue());
    }
    auto opened = SqliteStudioStore::open(databasePath(), projectId());
    ASSERT_TRUE(opened.hasValue());
    auto store = std::move(opened).value();
    const RecordingImportRecord record{
        .sessionId = sessionId(),
        .timelineId = TimelineId::create("timeline").value(),
        .base = TimestampNs{DurationNs::zero()},
        .importedRevision = 1,
        .importedAt = creator::core::Utc::parseRfc3339(
                          "2026-07-17T00:00:01Z").value()};

    EXPECT_FALSE(store.putRecordingImport(record).hasValue());
}

TEST_F(SqliteStudioStoreTest, ConcurrentExactImportRetriesAreBothSuccessful) {
    insertRecordingSession("COMPLETED");
    {
        auto connection = SqliteConnection::open(databasePath());
        ASSERT_TRUE(connection.hasValue());
        ASSERT_TRUE(connection.value()
                        .execute(
                            "INSERT INTO timelines(timeline_id,project_id,name,"
                            "frame_rate_numerator,frame_rate_denominator,revision,is_primary) "
                            "VALUES('timeline','project','Main',60,1,0,1)")
                        .hasValue());
    }
    auto firstOpened = SqliteStudioStore::open(databasePath(), projectId());
    auto secondOpened = SqliteStudioStore::open(databasePath(), projectId());
    ASSERT_TRUE(firstOpened.hasValue());
    ASSERT_TRUE(secondOpened.hasValue());
    auto first = std::move(firstOpened).value();
    auto second = std::move(secondOpened).value();
    const RecordingImportRecord record{
        .sessionId = sessionId(),
        .timelineId = TimelineId::create("timeline").value(),
        .base = TimestampNs{DurationNs{10}},
        .importedRevision = 1,
        .importedAt = creator::core::Utc::parseRfc3339(
                          "2026-07-17T00:00:01Z").value()};
    std::barrier ready{2};

    auto firstWrite = std::async(std::launch::async, [&] {
        ready.arrive_and_wait();
        return first.putRecordingImport(record);
    });
    auto secondWrite = std::async(std::launch::async, [&] {
        ready.arrive_and_wait();
        return second.putRecordingImport(record);
    });

    EXPECT_TRUE(firstWrite.get().hasValue());
    EXPECT_TRUE(secondWrite.get().hasValue());
    auto check = SqliteConnection::open(databasePath());
    ASSERT_TRUE(check.hasValue());
    EXPECT_EQ(check.value().scalarInt64("SELECT count(*) FROM recording_imports")
                  .value(),
              1);
}

}  // namespace
