# R1-06 Studio Scene, Source, and Recording Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver persistent Studio scenes and sources, live scene composition,
configurable action-parity shortcuts, recording markers/HUD, and an atomic,
idempotent source-recording-to-editor timeline workflow.

**Architecture:** A Qt-free scene domain and checksum-pinned SQLite migration
feed an asynchronous `StudioWorkflowController`. The existing recorder continues
source-separated capture; a new audited in-process media probe and deterministic
reconciler append completed sessions to the timeline as one composite edit.
Screen and camera preview items share the existing bounded native-frame path.

**Tech Stack:** C++20, Qt 6.8.3 Quick/Gui/QSettings, SQLite amalgamation,
audited dynamic FFmpeg 8.1.2, existing MLT 7.40.0 preview, CMake/Ninja,
GoogleTest.

## Global Constraints

- Do not reduce R1-06 to UI-only scene state; every accepted scene switch and
  marker must survive reopen and affect the imported timeline.
- Preserve independent screen, camera, microphone, and system-audio recordings;
  do not add a second program-output encode.
- All database, media probe, reconciliation, and settings persistence work runs
  off the UI thread.
- SQLite is authoritative; QML, native preview, MLT, and generated view models are
  disposable projections.
- Use `core::TimestampNs`, `core::DurationNs`, and exact `core::FrameRate`; raw
  integer time is allowed only at SQLite/Qt property boundaries.
- R1 source roles are exactly `Screen`, `Camera`, `Microphone`, and
  `SystemAudio`.
- Scene/source structure and transforms are editable only while idle; scene
  switching and marker placement remain enabled while recording.
- Scene activation is published only after its recording event is durable.
- Import is one transaction and one revision; a completed session is imported at
  most once, and failure leaves zero partial assets/clips/markers.
- Reuse the audited FFmpeg libraries in process; never launch `ffprobe`, `melt`,
  or another external media process.
- No new third-party dependency, GPL MLT module, GPL-enabled FFmpeg, web runtime,
  account service, or cloud dependency.
- Windows x64 is the physical gate. Preserve macOS source architecture and state
  its unavailable physical verification honestly.
- Keep user-owned root images untracked and unchanged.

---

### Task 1: Qt-free Studio scene domain

**Files:**
- Create: `src/domain/StudioScene.h`
- Create: `src/domain/StudioScene.cpp`
- Modify: `src/domain/CMakeLists.txt`
- Create: `tests/domain/StudioSceneTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `domain::SceneId`, `domain::SourceId`, `domain::VisualTransform`.
- Produces:
  - `enum class StudioSourceRole { Screen, Camera, Microphone, SystemAudio };`
  - `studioSourceRoleName(StudioSourceRole) -> std::string_view`
  - `studioSourceRoleFromName(std::string_view) -> Result<StudioSourceRole>`
  - `SceneSource::create(SourceId, StudioSourceRole, std::string, int32_t,
    bool, optional<VisualTransform>)`
  - `StudioScene::create(SceneId, std::string, int32_t,
    vector<SceneSource>)`
  - immutable add/remove/reorder/rename/set-source operations returning `Result`.

- [ ] **Step 1: Write failing value and invariant tests**

```cpp
TEST(StudioSceneTest, RejectsDuplicateRoleAndAudioTransform) {
    const auto screen = source("screen", StudioSourceRole::Screen, fullFrame());
    const auto duplicate = source("screen-2", StudioSourceRole::Screen,
                                  fullFrame());
    EXPECT_FALSE(StudioScene::create(sceneId("scene"), "강의", 0,
                                     {screen, duplicate}).hasValue());
    EXPECT_FALSE(SceneSource::create(sourceId("mic"),
                                     StudioSourceRole::Microphone, "마이크", 0,
                                     true, fullFrame()).hasValue());
}

TEST(StudioSceneTest, PresetsUseEditorVisualTransformExactly) {
    const auto scenes = defaultStudioScenes();
    ASSERT_EQ(scenes.size(), 3U);
    EXPECT_EQ(scenes[0].sources()[1].transform(),
              VisualTransform::create(0.70, 0.05, 0.25, 0.25, 1, 1, 0,
                                      0, 0, 0, 0, 1, 10).value());
}
```

- [ ] **Step 2: Run the focused tests and confirm red**

Run: `build/windows-debug/cs_tests.exe --gtest_filter=StudioSceneTest.*`

Expected: build fails because `StudioScene.h` does not exist.

- [ ] **Step 3: Implement bounded immutable values and defaults**

```cpp
enum class StudioSourceRole { Screen, Camera, Microphone, SystemAudio };

class SceneSource final {
public:
    static core::Result<SceneSource> create(
        SourceId id, StudioSourceRole role, std::string name,
        std::int32_t position, bool enabled,
        std::optional<VisualTransform> transform);
    [[nodiscard]] core::Result<SceneSource> withEnabled(bool) const;
    [[nodiscard]] core::Result<SceneSource> withTransform(
        std::optional<VisualTransform>) const;
};

class StudioScene final {
public:
    static core::Result<StudioScene> create(
        SceneId id, std::string name, std::int32_t position,
        std::vector<SceneSource> sources);
};

[[nodiscard]] core::Result<std::vector<StudioScene>> defaultStudioScenes();
```

Enforce non-empty IDs/names, byte limits already used by identifiers, positions
0–1023, one role per scene, transform only for video, and required transform for
enabled video.

- [ ] **Step 4: Run domain tests**

Run: `build/windows-debug/cs_tests.exe --gtest_filter=StudioSceneTest.*`

Expected: all `StudioSceneTest` tests pass.

- [ ] **Step 5: Commit**

```powershell
git add src/domain/StudioScene.* src/domain/CMakeLists.txt tests/domain/StudioSceneTest.cpp tests/CMakeLists.txt
git commit -m "feat(domain): model persistent studio scenes"
```

### Task 2: Migration 003 and durable Studio store

**Files:**
- Create: `src/project_store/migrations/003_studio_workflow.sql`
- Modify: `src/project_store/CMakeLists.txt`
- Modify: `src/project_store/MigrationRunner.h`
- Modify: `src/project_store/MigrationRunner.cpp`
- Create: `src/project_store/IStudioStore.h`
- Create: `src/project_store/SqliteStudioStore.h`
- Create: `src/project_store/SqliteStudioStore.cpp`
- Modify: `tests/project_store/MigrationRunnerTest.cpp`
- Create: `tests/project_store/SqliteStudioStoreTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 scene values, existing recording/timeline IDs, pinned
  `MigrationRunner`.
- Produces `StudioSnapshot`, `RecordingSourceRole`, `RecordingSceneEvent`,
  `RecordingMarker`, `RecordingImportRecord`, and `IStudioStore` load/mutation
  methods.

- [ ] **Step 1: Extend migration tests to require version 3**

```cpp
TEST(MigrationRunnerTest, AppliesStudioWorkflowMigrationTransactionally) {
    auto connection = freshConnection();
    ASSERT_TRUE(applyMigrations(connection, defaultMigrations()).hasValue());
    EXPECT_EQ(connection.scalarInt64(
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN "
        "('scenes','scene_sources','studio_state','recording_sources',"
        "'recording_scene_events','recording_markers','recording_imports')")
                  .value(), 7);
    EXPECT_EQ(connection.scalarInt64(
        "SELECT count(*) FROM schema_migrations WHERE version=3").value(), 1);
}
```

Add tests for 2→3 preservation, checksum tamper, failed-statement rollback, exact
role checks, finite/crop/opacity checks, foreign keys, and duplicate order/event
rejection.

- [ ] **Step 2: Run migration tests and confirm red**

Run: `build/windows-debug/cs_tests.exe --gtest_filter=MigrationRunnerTest.*`

Expected: version/table assertions fail because only migrations 001–002 exist.

- [ ] **Step 3: Add checksum-pinned migration 003**

The SQL creates the seven tables named in the design, uses `STRICT`-compatible
scalar types, explicit `CHECK` constraints, unique `(project_id, position)`,
`(scene_id, role)`, `(session_id, sequence)`, and all required foreign keys.
Update `MigrationRunner::kLatestVersion` to 3 and embed the SHA-256 of the exact
UTF-8 SQL bytes.

- [ ] **Step 4: Write failing Studio store tests**

```cpp
TEST(SqliteStudioStoreTest, SeedsLoadsAndMutatesUnicodeScenesAtomically) {
    auto store = openStore();
    ASSERT_TRUE(store.seedDefaultsIfEmpty(defaultStudioScenes().value()).hasValue());
    const auto before = store.load().value();
    ASSERT_TRUE(store.renameScene(before.scenes[0].id(), "새 강의").hasValue());
    EXPECT_EQ(store.load().value().scenes[0].name(), "새 강의");
}

TEST(SqliteStudioStoreTest, RecordingSwitchFailurePublishesNothing) {
    auto store = openStoreWithRejectTrigger("recording_scene_events");
    EXPECT_FALSE(store.recordSceneSwitch(sessionId(), sceneId(), 2,
                                         at(3'000'000'000)).hasValue());
    EXPECT_EQ(store.loadRecordingSceneEvents(sessionId()).value().size(), 0U);
}
```

- [ ] **Step 5: Implement `IStudioStore` and `SqliteStudioStore`**

```cpp
class IStudioStore {
public:
    virtual core::Result<void> seedDefaultsIfEmpty(
        const std::vector<domain::StudioScene>&) = 0;
    virtual core::Result<StudioSnapshot> load() = 0;
    virtual core::Result<void> commitSceneMutation(
        const StudioMutation&) = 0;
    virtual core::Result<void> prepareRecording(
        const domain::SessionId&, const std::vector<RecordingSourceRole>&,
        const domain::SceneId&) = 0;
    virtual core::Result<void> recordSceneSwitch(
        const domain::SessionId&, const domain::SceneId&, std::uint64_t,
        core::TimestampNs) = 0;
    virtual core::Result<void> recordMarker(const RecordingMarker&) = 0;
    virtual core::Result<std::vector<UnimportedRecording>>
        completedUnimportedRecordings() = 0;
    virtual ~IStudioStore() = default;
};
```

- [ ] **Step 6: Run focused migration/store tests**

Run: `build/windows-debug/cs_tests.exe --gtest_filter=MigrationRunnerTest.*:SqliteStudioStoreTest.*`

Expected: all pass and the v1/v2 preservation fixtures remain byte-identical.

- [ ] **Step 7: Commit**

```powershell
git add src/project_store tests/project_store tests/CMakeLists.txt
git commit -m "feat(project-store): persist studio scenes and recording events"
```

### Task 3: Timeline markers and composite recording import command

**Files:**
- Modify: `src/domain/TimelineTypes.h`
- Modify: `src/domain/TimelineTypes.cpp`
- Modify: `src/domain/Identifiers.h`
- Modify: `src/domain/Timeline.h`
- Modify: `src/domain/Timeline.cpp`
- Create: `src/domain/ImportRecordingCommand.h`
- Create: `src/domain/ImportRecordingCommand.cpp`
- Modify: `src/domain/EditCommandJson.cpp`
- Modify: `src/domain/CMakeLists.txt`
- Modify: `src/project_store/internal/EditCommandCodec.cpp`
- Modify: `src/project_store/SqliteTimelineStore.cpp`
- Modify: `tests/domain/TimelineTypesTest.cpp`
- Modify: `tests/domain/TimelineTest.cpp`
- Modify: `tests/domain/IdentifiersTest.cpp`
- Create: `tests/domain/ImportRecordingCommandTest.cpp`
- Modify: `tests/project_store/SqliteTimelineStoreTest.cpp`

**Interfaces:**
- Produces typed `MarkerId`, `TimelineMarker`, Timeline marker CRUD, and
  `ImportRecordingCommand::create(vector<Track>, vector<TimelineMarker>)` with
  exact execute/undo/redo serialization.

- [ ] **Step 1: Write marker and command round-trip tests**

```cpp
TEST(ImportRecordingCommandTest, ExecuteUndoRedoRestoresExactTimeline) {
    auto timeline = baseTimeline();
    const auto before = timeline;
    auto command = importCommandWithFourTracksAndFiveMarkers();
    ASSERT_TRUE(command->execute(timeline).hasValue());
    const auto imported = timeline;
    ASSERT_TRUE(command->undo(timeline).hasValue());
    EXPECT_EQ(timeline, before);
    ASSERT_TRUE(command->execute(timeline).hasValue());
    EXPECT_EQ(timeline, imported);
}
```

Cover duplicate marker IDs/positions, negative positions, empty labels, collision
with existing track/clip/marker identity, JSON UTF-8, and corrupt undo payload.

- [ ] **Step 2: Run focused tests and confirm red**

Run: `build/windows-debug/cs_tests.exe --gtest_filter=Timeline*Test.*:ImportRecordingCommandTest.*`

Expected: missing marker/import types fail compilation.

- [ ] **Step 3: Implement marker aggregate support and import command**

```cpp
class TimelineMarker final {
public:
    static core::Result<TimelineMarker> create(
        MarkerId id, core::TimestampNs position, std::string label);
};

class ImportRecordingCommand final : public IEditCommand {
public:
    static core::Result<std::unique_ptr<ImportRecordingCommand>> create(
        CommandId, std::vector<Track>, std::vector<TimelineMarker>);
    core::Result<void> execute(Timeline&) override;
    core::Result<void> undo(Timeline&) override;
    EditCommandRecord record() const override;
};
```

The command owns exact inserted track/marker values and rejects every collision
before mutating the first item.

- [ ] **Step 4: Load/store markers and command history**

Extend snapshot writes and loads to the existing `markers` table and add codec
dispatch for `IMPORT_RECORDING`. Existing timelines with zero markers remain
equal.

- [ ] **Step 5: Run domain/store regression tests**

Run: `build/windows-debug/cs_tests.exe --gtest_filter=Timeline*Test.*:ImportRecordingCommandTest.*:SqliteTimelineStoreTest.*`

Expected: all pass.

- [ ] **Step 6: Commit**

```powershell
git add src/domain src/project_store tests/domain tests/project_store
git commit -m "feat(domain): add recording import and timeline markers"
```

### Task 4: Audited in-process FFmpeg media probe

**Files:**
- Create: `src/core/Sha256.h`
- Create: `src/core/Sha256.cpp`
- Modify: `src/core/CMakeLists.txt`
- Modify: `src/mlt_adapter/MltRuntimeManifest.cpp`
- Create: `src/media/IMediaProbe.h`
- Modify: `src/media/CMakeLists.txt`
- Create: `src/ffmpeg_adapter/FfmpegMediaProbe.h`
- Create: `src/ffmpeg_adapter/FfmpegMediaProbe.cpp`
- Modify: `src/ffmpeg_adapter/CMakeLists.txt`
- Create: `tests/fakes/FakeMediaProbe.h`
- Create: `tests/core/Sha256Test.cpp`
- Create: `tests/ffmpeg_adapter/FfmpegMediaProbeTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces `MediaProbeResult` with exact duration, optional video/audio metadata,
  format/codec identity, byte size, and lowercase SHA-256.

- [ ] **Step 1: Write failing real-container and malicious-path tests**

Create deterministic 64×36 H.264/MPEG-4 fixture and mono AAC fixture with the
existing audited encoders. Assert exact stream kind, dimensions/rate or
sample-rate/channels, bounded duration tolerance of one frame/sample block,
non-empty codec/format, byte size, and independently computed SHA-256. Also test
missing, directory, symlink/reparse, traversal, zero-stream, and truncated input.

- [ ] **Step 2: Run and confirm red**

Run: `build/windows-debug/cs_ffmpeg_tests.exe --gtest_filter=FfmpegMediaProbeTest.*`

Expected: missing `FfmpegMediaProbe` fails compilation.

- [ ] **Step 3: Implement RAII libavformat probing**

```cpp
struct MediaProbeResult final {
    core::DurationNs duration;
    std::optional<domain::VideoAssetMetadata> video;
    std::optional<domain::AudioAssetMetadata> audio;
    std::string formatName;
    std::string codecName;
    std::uint64_t byteSize;
    std::string sha256;
};

class IMediaProbe {
public:
    virtual core::Result<MediaProbeResult> probe(
        const std::filesystem::path& packageRoot,
        const std::filesystem::path& relativePath) = 0;
    virtual ~IMediaProbe() = default;
};
```

Extract the already verified streaming SHA-256 implementation from
`MltRuntimeManifest.cpp` into Qt-free `core::Sha256`/`core::sha256File`, preserving
the runtime manifest vectors. Resolve the media path with the repository
safe-relative-path helper before
`avformat_open_input`, select exactly one supported primary stream, use exact
rational conversion with overflow checks, close every context on all exits, and
hash the already validated regular file.

- [ ] **Step 4: Run FFmpeg tests repeatedly**

Run: `1..5 | % { build/windows-debug/cs_ffmpeg_tests.exe --gtest_filter=FfmpegMediaProbeTest.*; if ($LASTEXITCODE) { exit $LASTEXITCODE } }`

Expected: five passes and every fixture directory removes successfully.

- [ ] **Step 5: Commit**

```powershell
git add src/core src/media src/mlt_adapter src/ffmpeg_adapter tests/core tests/fakes tests/ffmpeg_adapter tests/CMakeLists.txt
git commit -m "feat(ffmpeg): probe recorded media in process"
```

### Task 5: Deterministic recording import planner

**Files:**
- Create: `src/app/RecordingImportPlanner.h`
- Create: `src/app/RecordingImportPlanner.cpp`
- Modify: `src/app/CMakeLists.txt`
- Create: `tests/app/RecordingImportPlannerTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes completed session segments, source roles, scene snapshots/events,
  markers, current timeline, and `MediaProbeResult`s.
- Produces `RecordingImportPlan { sessionId, appendBase, assets, tracks,
  markers }` and deterministic IDs.

- [ ] **Step 1: Write table-driven boundary tests**

```cpp
TEST(RecordingImportPlannerTest, SplitsVideoAtSceneBoundariesExactly) {
    const auto plan = planRecording(
        segments({{0_s, 2_s}, {2_s, 2_s}}),
        events({{0_s, presentation()}, {1500_ms, screenOnly()},
                {3_s, cameraOnly()}}));
    EXPECT_EQ(ranges(plan.track("camera")),
              (std::vector{range(0_s, 1500_ms), range(1500_ms, 1500_ms),
                           range(3_s, 1_s)}));
    EXPECT_FALSE(plan.track("camera").clips()[1].enabled());
}
```

Cover switch exactly at segment boundary, multiple switches within one segment,
disabled audio, gaps/failed segments, events out of order, missing role/probe,
timeline append offset, marker offset, Unicode IDs, overflow, and deterministic
repeat equality.

- [ ] **Step 2: Run and confirm red**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=RecordingImportPlannerTest.*`

Expected: missing planner fails compilation.

- [ ] **Step 3: Implement pure planning**

Build one track per observed role, preserve segment/source offsets, split only at
strictly interior event boundaries, copy the active scene's exact enabled/
transform state, leave gaps unfilled, sort deterministically, and create five
timeline markers at `appendBase + relativePosition`.

- [ ] **Step 4: Run focused tests**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=RecordingImportPlannerTest.*`

Expected: all pass with no filesystem/SQLite/Qt event loop dependency.

- [ ] **Step 5: Commit**

```powershell
git add src/app/RecordingImportPlanner.* src/app/CMakeLists.txt tests/app/RecordingImportPlannerTest.cpp tests/CMakeLists.txt
git commit -m "feat(app): plan deterministic recording imports"
```

### Task 6: Atomic idempotent timeline reconciliation

**Files:**
- Modify: `src/project_store/ITimelineStore.h`
- Modify: `src/project_store/SqliteTimelineStore.h`
- Modify: `src/project_store/SqliteTimelineStore.cpp`
- Modify: `src/app/TimelineEditService.h`
- Modify: `src/app/TimelineEditService.cpp`
- Create: `src/app/RecordingTimelineReconciler.h`
- Create: `src/app/RecordingTimelineReconciler.cpp`
- Modify: `src/app/CMakeLists.txt`
- Modify: `tests/app/TimelineEditServiceTest.cpp`
- Create: `tests/app/RecordingTimelineReconcilerTest.cpp`
- Modify: `tests/project_store/SqliteTimelineStoreTest.cpp`

**Interfaces:**
- Adds `TimelineCommit::assetsToInsert` and optional
  `RecordingImportRecord importRecord`.
- Adds `TimelineEditService::executeRecordingImport(command, assets, record)`.
- Produces `RecordingTimelineReconciler::reconcile(packageRoot, sessionId)`.
- Produces `IRecordingTimelineReconciler` so controller tests never construct
  SQLite or FFmpeg dependencies.

- [ ] **Step 1: Write failing atomicity/idempotency tests**

For each SQL statement boundary, inject a trigger failure and assert unchanged
revision/history/timeline/assets/markers and absent import row. Then retry without
the trigger and assert exactly one import. Call a third time and assert successful
no-op with byte-equal state. Undo/redo must remove/restore clips and markers while
assets/import row remain.

- [ ] **Step 2: Run and confirm red**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=TimelineEditServiceTest.*:RecordingTimelineReconcilerTest.*`

Expected: atomic import API is missing.

- [ ] **Step 3: Extend commit transaction**

```cpp
struct TimelineCommit final {
    domain::Timeline snapshot;
    std::int64_t expectedRevision;
    EditEventRecord event;
    std::size_t historyCount;
    std::size_t historyCursor;
    std::optional<std::size_t> cleanCursor;
    std::vector<domain::MediaAsset> assetsToInsert;
    std::optional<RecordingImportRecord> importRecord;
};
```

`SqliteTimelineStore::commitEdit` begins one transaction, checks the import key,
inserts new immutable assets, writes snapshot/event/checkpoint/import row, and
commits. Existing edit commits pass empty extras and retain behavior.

- [ ] **Step 4: Implement reconciliation orchestration**

Open the package/store, load completed-unimported data, probe every READY segment,
build the Task 5 plan, create `ImportRecordingCommand`, and call
`executeRecordingImport`. Treat an already present import row as success. Keep
probe work outside the SQLite write transaction but revalidate file identity
before commit.

```cpp
class IRecordingTimelineReconciler {
public:
    virtual core::Result<RecordingReconcileResult> reconcile(
        const std::filesystem::path&, const domain::SessionId&) = 0;
    virtual ~IRecordingTimelineReconciler() = default;
};
```

- [ ] **Step 5: Run focused and store tests**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=TimelineEditServiceTest.*:RecordingTimelineReconcilerTest.*`

Run: `build/windows-debug/cs_tests.exe --gtest_filter=SqliteTimelineStoreTest.*`

Expected: all pass.

- [ ] **Step 6: Commit**

```powershell
git add src/project_store src/app tests/app tests/project_store
git commit -m "feat(editor): reconcile completed recordings atomically"
```

### Task 7: Asynchronous Studio workflow controller and models

**Files:**
- Create: `src/app/StudioSceneModel.h`
- Create: `src/app/StudioSceneModel.cpp`
- Create: `src/app/StudioSourceModel.h`
- Create: `src/app/StudioSourceModel.cpp`
- Create: `src/app/StudioWorkflowTypes.h`
- Create: `src/app/StudioWorkflowWorker.h`
- Create: `src/app/StudioWorkflowWorker.cpp`
- Create: `src/app/StudioWorkflowController.h`
- Create: `src/app/StudioWorkflowController.cpp`
- Modify: `src/app/CMakeLists.txt`
- Create: `tests/app/StudioWorkflowControllerTest.cpp`
- Modify: `tests/app/EditorModelsTest.cpp`

**Interfaces:**
- QML properties: scene/source models, busy, recording, reconciling, selected and
  active IDs, selected transform, marker count, status.
- Invokables: add/duplicate/rename/remove/move/select/switch scene;
  select/toggle/move source; set/reset/PIP transform; add marker; prepare/abort/
  complete recording; reopen project.

- [ ] **Step 1: Write controller state-machine tests with fake stores/probe**

```cpp
TEST(StudioWorkflowControllerTest, RecordingSwitchPublishesOnlyAfterPersistence) {
    DeferredStudioStore store;
    StudioWorkflowController controller{store.factory(), fakeReconciler()};
    openReady(controller);
    controller.prepareRecording(sessionId(), activeSources(), at(10_s));
    store.finishSuccess();
    controller.switchScene("screen-scene");
    EXPECT_EQ(controller.activeSceneId(), "presentation-scene");
    store.finishSuccess();
    EXPECT_EQ(controller.activeSceneId(), "screen-scene");
}
```

Cover project generation, seed failure, stale callbacks, every edit guard,
recording position, event sequence, marker failure, reconcile retry, model stable
roles, repeated open/close, and destructor thread/file release.

- [ ] **Step 2: Run and confirm red**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=StudioWorkflowControllerTest.*:EditorModelsTest.*`

Expected: missing workflow types fail compilation.

- [ ] **Step 3: Implement immutable model publication and worker serialization**

Use the Editor controller generation/worker pattern. Each worker result carries a
complete `StudioWorkflowState`; the GUI thread swaps models and emits the minimum
property signals. No model is changed before durable success.

- [ ] **Step 4: Run controller/model tests**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=StudioWorkflowControllerTest.*:EditorModelsTest.*`

Expected: all pass, including event-loop responsiveness under delayed store/probe.

- [ ] **Step 5: Commit**

```powershell
git add src/app tests/app
git commit -m "feat(studio): coordinate persistent scene workflows"
```

### Task 8: Bind live recording preparation, completion, and editor refresh

**Files:**
- Modify: `src/app/LiveRecordingController.h`
- Modify: `src/app/LiveRecordingController.cpp`
- Create: `src/app/StudioRecordingBinding.h`
- Create: `src/app/StudioRecordingBinding.cpp`
- Modify: `src/app/ProjectEditorBinding.h`
- Modify: `src/app/ProjectEditorBinding.cpp`
- Modify: `src/main.cpp`
- Modify: `tests/app/LiveRecordingControllerTest.cpp`
- Create: `tests/app/StudioRecordingBindingTest.cpp`
- Modify: `tests/app/ProjectControllerTest.cpp`

**Interfaces:**
- `LiveRecordingController::RecordingPreparation` callback runs after durable
  `begin` and before engine start.
- Exposes active session ID/start/relative position and emits recording committed.
- `StudioRecordingBinding` supplies concrete source-role mappings, triggers
  reconcile after persistence, and refreshes Editor only after reconcile success.

- [ ] **Step 1: Write ordering and failure tests**

Assert exact call order `begin → studio prepare → engine start → engine complete →
project complete → reconcile → editor reopen`. Preparation failure must skip
engine start and abort the session. Reconcile failure must keep completed media,
leave Editor unchanged, and retry on project reopen.

- [ ] **Step 2: Run and confirm red**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=LiveRecordingControllerTest.*:StudioRecordingBindingTest.*:ProjectControllerTest.*`

Expected: preparation/binding APIs are missing.

- [ ] **Step 3: Implement lifecycle extension and binding**

```cpp
using RecordingPreparation = std::function<void(
    const LiveRecordingStart&, std::function<void(core::Result<void>)>)>;

[[nodiscard]] std::optional<domain::SessionId> activeSessionId() const;
[[nodiscard]] std::optional<core::TimestampNs> recordingPosition() const;
```

Guard all async callbacks with `QPointer` and active generation/session identity.
Capture concrete source IDs from screen/device controllers only in the binding;
store logical scene roles, never native handles in scenes.

- [ ] **Step 4: Run binding/controller tests**

Run: same focused command as Step 2.

Expected: all pass.

- [ ] **Step 5: Commit**

```powershell
git add src/app src/main.cpp tests/app
git commit -m "feat(recording): connect studio sessions to editor imports"
```

### Task 9: Shared screen/camera native preview composition

**Files:**
- Create: `src/app/VideoPreviewItem.h`
- Create: `src/app/VideoPreviewItem.cpp`
- Create: `src/app/VideoPreviewItem_macos.mm`
- Create: `src/app/VideoPreviewItem_stub.cpp`
- Modify: `src/app/ScreenPreviewItem.h`
- Modify: `src/app/ScreenPreviewItem.cpp`
- Delete: `src/app/ScreenPreviewItem_macos.mm`
- Delete: `src/app/ScreenPreviewItem_stub.cpp`
- Create: `src/app/CameraPreviewItem.h`
- Create: `src/app/CameraPreviewItem.cpp`
- Modify: `src/app/DeviceCaptureController.h`
- Modify: `src/app/CMakeLists.txt`
- Modify: `src/main.cpp`
- Modify: `tests/app/DeviceCaptureControllerTest.cpp`
- Create: `tests/app/VideoPreviewItemTest.cpp`

**Interfaces:**
- `VideoPreviewItem` owns render state and platform `updatePaintNode`.
- Screen/camera subclasses provide `shared_ptr<LatestVideoFrameMailbox>`.
- `DeviceCaptureController::cameraPreviewMailbox()` exposes the primary bounded
  mailbox without exposing a capture source.

- [ ] **Step 1: Write mailbox/lifetime/render-state tests**

Verify controller replacement disconnects old signals, camera preview consumes
latest frames without touching secondary recording sink, source visibility does
not stop capture, and item destruction releases retained handles. Preserve all
existing PreviewGeometry tests.

- [ ] **Step 2: Run and confirm red**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=VideoPreviewItemTest.*:DeviceCaptureControllerTest.*:PreviewGeometryTest.*`

Expected: missing generic/camera item fails compilation.

- [ ] **Step 3: Extract the existing platform node without behavior changes**

Move the Metal/IOSurface node to `VideoPreviewItem_macos.mm`, replace screen-only
messages with role-neutral video messages, keep zero-copy retained-frame lifetime,
and keep the explicit unsupported stub. Screen and camera subclasses only return
their mailbox and connect update signals.

- [ ] **Step 4: Run focused and QML smoke tests**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=VideoPreviewItemTest.*:DeviceCaptureControllerTest.*:PreviewGeometryTest.*:QmlSmokeTest.*`

Expected: all pass; unsupported Windows remains labelled, not synthetic capture.

- [ ] **Step 5: Commit**

```powershell
git add src/app src/main.cpp tests/app
git commit -m "feat(studio-ui): compose screen and camera preview sources"
```

### Task 10: Configurable shortcuts with shared Action parity

**Files:**
- Create: `src/app/ShortcutSettingsController.h`
- Create: `src/app/ShortcutSettingsController.cpp`
- Modify: `src/app/CMakeLists.txt`
- Modify: `src/main.cpp`
- Modify: `qml/Main.qml`
- Modify: `qml/StudioPage.qml`
- Create: `tests/app/ShortcutSettingsControllerTest.cpp`
- Modify: `tests/app/QmlSmokeTest.cpp`

**Interfaces:**
- Properties for record, marker, previous/next, and nine direct-scene sequences.
- `setShortcut(actionId, sequence)` validates/persists asynchronously.
- QML `Action`s are shared by visible controls and `Shortcut`s.

- [ ] **Step 1: Write validation/persistence/parity tests**

Test defaults, Unicode settings path, invalid/empty, duplicates, the six reserved
sequences, async write failure, reopen persistence, and that invoking each visible
Action and shortcut Action produces identical fake-controller calls/state guards.

- [ ] **Step 2: Run and confirm red**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=ShortcutSettingsControllerTest.*:QmlSmokeTest.*Shortcut*`

Expected: missing settings controller/actions fail.

- [ ] **Step 3: Implement settings controller and Actions**

Use canonical `QKeySequence::PortableText`, one worker-backed QSettings group
`StudioShortcuts/v1`, reject conflicts before queuing persistence, and publish
only after success. `Action.enabled` is the one state guard for its button and
shortcut.

- [ ] **Step 4: Run focused tests**

Run: same command as Step 2.

Expected: all pass.

- [ ] **Step 5: Commit**

```powershell
git add src/app src/main.cpp qml tests/app
git commit -m "feat(studio): add configurable action-parity shortcuts"
```

### Task 11: Complete Studio scene/source inspector and truthful HUD

**Files:**
- Modify: `qml/StudioPage.qml`
- Modify: `qml/Main.qml`
- Modify: `tests/app/QmlSmokeTest.cpp`
- Modify: `tests/app/StudioWorkflowControllerTest.cpp`

**Interfaces:**
- Consumes Tasks 7–10 controllers/models/actions.
- Produces real scene/source panels, transform/PIP inspector, active composition,
  marker controls, shortcut editor, and complete HUD.

- [ ] **Step 1: Extend QML smoke/accessibility tests first**

Load 1280×720, 1440×900, and 200 percent scale fixtures. Query the real
`QAccessibleInterface` tree. Assert controls, names, descriptions, tab order,
idle/recording locks, active-scene switching, numeric validation, marker/action
parity, camera/screen layering, HUD unavailable labels, reconciliation state, and
zero binding/type warnings.

- [ ] **Step 2: Run and confirm red**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=QmlSmokeTest.*Studio*`

Expected: current static model/disabled inspector fails the new assertions.

- [ ] **Step 3: Replace static Studio scaffolding with model-driven UI**

Bind scene/source delegates to their typed models. Add add/duplicate/rename/
remove/reorder controls, enabled toggles, video transform numeric fields and PIP
presets, screen/camera preview layers, marker button, shortcut editor, and HUD
fields from the design. Do not hide failures or label test patterns as capture.

- [ ] **Step 4: Run QML and controller tests**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=QmlSmokeTest.*:StudioWorkflowControllerTest.*`

Expected: all pass with zero QML warnings.

- [ ] **Step 5: Launch the app and inspect all three pages**

Run `build/windows-debug/creator_studio.exe`, create/open a Unicode project, and
verify Home→Studio→Editor navigation, scene/source controls, inspector validation,
shortcut editor, and honest unsupported-capture labels. Save only new R1-06
evidence images under `docs/superpowers/reports/assets/`; never overwrite the
three untracked root images.

- [ ] **Step 6: Commit**

```powershell
git add qml tests/app docs/superpowers/reports/assets
git commit -m "feat(studio-ui): deliver scene workflow and recording hud"
```

### Task 12: Physical acceptance, clean gate, review, and integration

**Files:**
- Create: `tests/acceptance/R1StudioWorkflowAcceptanceTest.cpp`
- Modify: `tests/CMakeLists.txt`
- Create: `docs/superpowers/reports/2026-07-17-r1-06-verification.md`
- Modify: `IMPLEMENTATION_ROADMAP.md`
- Modify: this plan checklist.

**Interfaces:**
- Produces one deadline-bounded physical executable covering Unicode scenes,
  source recording reconciliation, failures, reopen, MLT pixels/PCM, UI
  responsiveness, parallel paths, and resource cleanup.

- [ ] **Step 1: Implement the real multi-source workflow acceptance**

Use a real Unicode `.cstudio`, SQLite, `FfmpegLiveRecordingEngine` with
deterministic fake live-capture bindings that encode real media segments, Studio
controller/worker, reconciliation, Editor controller/worker, and audited MLT.
Persist three scenes, at least three switches and five markers, import four roles,
destroy all objects, reopen, compare exact rows/models/assets/tracks/split clips/
transforms/markers, request representative scene frames and mixed PCM, undo/redo
the composite import, and prove a second reconcile is byte-equal no-op.

- [ ] **Step 2: Implement repeated failure and cleanup acceptance**

Repeat SQLite prepare/switch/marker/import failure, media probe failure,
missing/replaced/reparse media, stale callback, and controller destruction. Assert
zero partial import, recoverable completed recording, successful retry where
valid, and package removal after every process-local object dies. Use PID-qualified
temp roots and per-discovered-test 60-second CTest deadlines.

- [ ] **Step 3: Prove responsiveness and bounded scale**

Accelerate a 30-minute planner/store fixture with 120 contiguous 15-second
segments, 60 scene switches, and 300 markers. Drive actual Studio and Editor
worker paths, record probe/plan/
transaction/graph/frame timings, maximum UI gap, branch/clip counts, handle count,
and working-set delta. Require maximum UI gap below 250 ms and preserve the
R1-05 graph/frame/memory budgets.

- [ ] **Step 4: Run focused acceptance five times in parallel-capable mode**

Run source scan and `git diff --check`, then:

```powershell
1..5 | % {
  ctest --test-dir build/windows-mlt-audit -R R1StudioWorkflowAcceptanceTest `
        --output-on-failure -j4 --no-tests=error
  if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
```

Expected: every discovered physical test passes all five repetitions with no
skips/disabled tests, package collision, lock, or deadline overrun.

- [ ] **Step 5: Run fresh `/WX` build, full CTest, app, and shipping audits**

Configure a fresh `build/windows-r1-06-audit` against the stable
`build/mlt/prefix` with tests and MLT enabled and warnings as errors. Build every
target; run complete sequential CTest; launch app hidden and require
`Responding=True`; inspect app link inputs, direct/delay imports, staged runtime,
forbidden modules/processes, and MLT/FFmpeg manifests. Expected: zero compiler
warnings, zero test failures/skips, no test/fake link input, no external
`ffprobe`/`melt`, and only audited dynamic runtime files.

- [ ] **Step 6: Independent review and closure**

Use `superpowers:requesting-code-review`, fix every confirmed finding with a
regression test, rerun focused plus complete gates, write exact evidence and
platform/license limits, and mark only R1-06 complete.

- [ ] **Step 7: Commit and integrate**

```powershell
git add tests/acceptance tests/CMakeLists.txt IMPLEMENTATION_ROADMAP.md `
        docs/superpowers/plans/2026-07-17-r1-06-studio-workflow.md `
        docs/superpowers/reports/2026-07-17-r1-06-verification.md
git commit -m "docs(r1-06): record studio workflow verification"
```

Use `superpowers:finishing-a-development-branch` to fast-forward into
`feat/r1-usable-recorder-editor`, preserve user files, run the full integrated
gate again, clean the owned worktree/branch, and continue directly to R1-07.
