# R1-04 Durable Edit Operations Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship selectable split/trim/lift/ripple editing with transactional undo/redo/save and exact `.cstudio` reopen.

**Architecture:** A dedicated `EditorSessionWorker` owns `SqliteTimelineStore` and `TimelineEditService` off the UI thread. `EditorController` publishes only committed worker results, then synchronizes the disposable MLT graph; QML sends identity- and time-based product commands through the controller.

**Tech Stack:** C++20, Qt 6 Core/Quick/QML, SQLite, existing timeline domain commands, GoogleTest, CMake, MSVC `/W4 /permissive- /WX`.

## Global Constraints

- Preserve the full R1-R4 product scope; this slice is delivery item 4, not an MVP fork.
- SQLite, manifest loading, command replay, and MLT work never run on the UI thread.
- Durable domain commit precedes model publication and MLT synchronization.
- A failed commit changes neither controller state nor preview graph.
- Package/database paths fail closed on escape, link, reparse point, identity mismatch, or future schema.
- One durable edit and one MLT frame request may be in flight; stale generations are discarded.
- Use existing `SplitClipCommand`, `TrimClipCommand`, `DeleteRangeCommand`, and `TimelineEditService`; do not duplicate edit rules in QML.
- Keep Qt out of domain, project-store, edit-engine, and MLT graph-plan public interfaces.

---

### Task 1: Durable history capabilities and default primary timeline

**Files:**
- Modify: `src/domain/EditHistory.h`
- Modify: `src/app/TimelineEditService.h`
- Create: `src/app/EditorSessionTypes.h`
- Test: `tests/app/TimelineEditServiceTest.cpp`

**Interfaces:**
- Produces: `TimelineEditService::canUndo()`, `canRedo()`, and `historyCursor()`.
- Produces: Qt-free `EditorSessionState` and `EditorEditRequest` values consumed by the worker.

- [ ] **Step 1: Write failing capability tests**

```cpp
EXPECT_FALSE(service.canUndo());
EXPECT_FALSE(service.canRedo());
ASSERT_TRUE(service.execute(splitCommand()).hasValue());
EXPECT_TRUE(service.canUndo());
EXPECT_FALSE(service.canRedo());
ASSERT_TRUE(service.undo().hasValue());
EXPECT_FALSE(service.canUndo());
EXPECT_TRUE(service.canRedo());
```

- [ ] **Step 2: Run the focused test and confirm the missing API failure**

Run: `cmake --build build/windows-debug --target cs_tests -j 2` then `build/windows-debug/cs_tests.exe --gtest_filter=TimelineEditServiceTest.ExposesDurableHistoryCapabilities`

Expected: compile failure naming `canUndo` before implementation.

- [ ] **Step 3: Add exact state contracts**

```cpp
enum class EditorEditKind { Split, TrimLeading, TrimTrailing, DeleteRange, Undo, Redo, Save };

struct EditorEditRequest final {
    EditorEditKind kind;
    std::optional<domain::TrackId> trackId;
    std::optional<domain::ClipId> clipId;
    core::TimestampNs position{};
    std::optional<domain::TimeRange> range;
    bool ripple{false};
};

struct EditorSessionState final {
    std::vector<domain::MediaAsset> assets;
    edit_engine::TimelineSnapshot snapshot;
    bool canUndo{false};
    bool canRedo{false};
    bool clean{true};
};
```

Add `EditHistory::canUndo() { return cursor_ > 0; }` and
`canRedo() { return cursor_ < commands_.size(); }`; delegate through
`TimelineEditService` without exposing its mutable history.

- [ ] **Step 4: Run all Qt-free timeline/domain tests**

Run: `build/windows-debug/cs_tests.exe --gtest_filter=TimelineEditServiceTest.*:EditHistoryTest.*`

Expected: all selected tests pass.

- [ ] **Step 5: Commit**

```powershell
git add src/domain/EditHistory.h src/app/TimelineEditService.h src/app/EditorSessionTypes.h tests/app/TimelineEditServiceTest.cpp
git commit -m "feat(editor): expose durable edit session state"
```

### Task 2: Project-owned editor session worker

**Files:**
- Create: `src/app/EditorSessionWorker.h`
- Create: `src/app/EditorSessionWorker.cpp`
- Modify: `src/app/CMakeLists.txt`
- Create: `tests/app/EditorSessionWorkerTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `EditorEditRequest`, `JsonProjectStore`, `SqliteTimelineStore`, and `TimelineEditService`.
- Produces signals `opened(generation, result)` and `edited(generation, commandId, result)` where `result` is `std::shared_ptr<const EditorSessionResult>`.

- [ ] **Step 1: Write failing real-package worker tests**

Create a Unicode temporary `.cstudio`, migrate it with `ProjectPackageStore`,
invoke the worker on a `QThread`, and assert:

```cpp
ASSERT_TRUE(result->state.has_value()) << result->error.message();
EXPECT_EQ(result->state->snapshot.packageRoot, package.path());
EXPECT_EQ(result->state->snapshot.timeline.name(), "Main");
EXPECT_TRUE(result->state->clean);
```

Also replace `project.db` with a directory junction after package validation and
assert the worker returns `InvalidArgument` without opening SQLite.

- [ ] **Step 2: Run and confirm the worker type is missing**

Run: `cmake --build build/windows-debug --target cs_app_tests -j 2`

Expected: compile failure for `EditorSessionWorker`.

- [ ] **Step 3: Implement fail-closed open**

`openProject(generation, packageRoot)` must:

```cpp
JsonProjectStore manifestStore;
auto manifest = manifestStore.load(packageRoot);
auto database = validatedPackageFile(packageRoot, manifest.value().databaseName);
auto store = SqliteTimelineStore::open(database.value(), manifest.value().projectId);
```

If `loadPrimaryTimeline()` returns `NotFound`, create `Main` at 60/1 with stable
unlocked `video-1` and `audio-1` tracks, then open `TimelineEditService` with a
100-command history limit. Generate command/event/clip identities with
`core::Uuid::generate()`; never accept identities from QML for new entities.

- [ ] **Step 4: Implement command execution and result construction**

Map requests exactly:

```cpp
SplitClipCommand(commandId, trackId, clipId, rightClipId, position)
TrimClipCommand(commandId, trackId, clipId, TrimEdge::Leading, position)
TrimClipCommand(commandId, trackId, clipId, TrimEdge::Trailing, position)
DeleteRangeCommand(commandId, *range, ripple, generatedRightClipIds)
```

Generate one right-clip identity for every enabled clip that strictly spans a
delete-range boundary. After success return the new snapshot plus a
`TimelineChangeSet` based on the old revision. Undo/redo mark all tracks affected
and request a full graph rebuild; split/trim identify one affected track.

- [ ] **Step 5: Prove commit failure atomicity and reopen**

Inject an `ITimelineStore` whose `commitEdit` fails. Assert byte-equivalent
`EditorSessionState` before/after and no success signal. With real SQLite,
destroy the worker, reopen, and compare timeline, revision, history cursor,
clean state, `canUndo`, and `canRedo`.

- [ ] **Step 6: Run worker and store suites**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=EditorSessionWorkerTest.*` and `build/windows-debug/cs_tests.exe --gtest_filter=SqliteTimelineStoreTest.*:TimelineEditServiceTest.*`

Expected: all selected tests pass.

- [ ] **Step 7: Commit**

```powershell
git add src/app/EditorSessionWorker.* src/app/CMakeLists.txt tests/app/EditorSessionWorkerTest.cpp tests/CMakeLists.txt
git commit -m "feat(editor): own durable project sessions off UI thread"
```

### Task 3: Controller selection, ranges, and durable command pipeline

**Files:**
- Modify: `src/app/EditorController.h`
- Modify: `src/app/EditorController.cpp`
- Modify: `tests/app/EditorControllerTest.cpp`

**Interfaces:**
- Produces QML properties: `sessionBusy`, `selectedTrackId`, `selectedClipId`, `rangeInNs`, `rangeOutNs`, `hasMarkedRange`, `canUndo`, `canRedo`, `clean`.
- Produces invokables: `openProject(QUrl)`, `selectClip(QString,QString)`, `markRangeIn()`, `markRangeOut()`, `splitSelected()`, `trimSelectedStart()`, `trimSelectedEnd()`, `deleteMarkedRange(bool)`, `undo()`, `redo()`, `save()`.

- [ ] **Step 1: Write failing controller state-machine tests**

Cover invalid selection, boundary split, reversed marker entry normalization,
zero-length range rejection, one edit in flight, playback pause-before-edit,
late project-generation result rejection, and selection clearing when ripple
deletes the selected clip.

```cpp
controller.selectClip("video-1", "clip-1");
controller.seek(2'000'000'000);
controller.splitSelected();
EXPECT_TRUE(controller.sessionBusy());
controller.splitSelected();
EXPECT_EQ(worker->requests().size(), 1U);
```

- [ ] **Step 2: Run the focused controller suite and confirm missing properties**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=EditorControllerDurableTest.*`

Expected: compile failures for the new controller contract.

- [ ] **Step 3: Add the session worker thread and generation-safe queue**

Construct and start `sessionThread_` beside `workerThread_`. `openProject` bumps
the generation, clears old selection/range/preview, and posts exactly one open.
Durable commands receive monotonic command IDs and are rejected while
`sessionBusy_` is true. Destruction drains/invalidates both workers without a
callback into a destroyed controller.

- [ ] **Step 4: Publish commit before engine update**

On an accepted success result:

```cpp
mediaBinModel_.setAssets(state.assets);
timelineTrackModel_.setTimeline(state.snapshot.timeline);
snapshot_ = state.snapshot;
emit timelineChanged();
queueUpdate(*result.change);
```

If incremental MLT update fails, preserve the durable state, set preview stale,
and queue `load(snapshot_, true)`. Do not decrement busy counters twice when the
fallback completes.

- [ ] **Step 5: Run controller tests including existing playback coverage**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=EditorControllerTest.*:EditorControllerDurableTest.*`

Expected: all selected tests pass and maximum durable commands in flight is 1.

- [ ] **Step 6: Commit**

```powershell
git add src/app/EditorController.* tests/app/EditorControllerTest.cpp
git commit -m "feat(editor): serialize durable timeline commands"
```

### Task 4: Selectable editing UI and shortcut parity

**Files:**
- Modify: `qml/EditorPage.qml`
- Modify: `tests/app/QmlSmokeTest.cpp`

**Interfaces:**
- Consumes only the Task 3 controller properties/invokables.
- Produces stable object names for every control and selected clip.

- [ ] **Step 1: Write failing QML smoke tests**

Find `editorSplitButton`, `editorTrimStartButton`, `editorTrimEndButton`,
`editorUndoButton`, `editorRedoButton`, `editorSaveButton`, `editorMarkInButton`,
`editorMarkOutButton`, `editorLiftButton`, and `editorRippleDeleteButton`.
Click a clip and assert the fake receives its track/clip identities. Invoke each
button and its shortcut and assert both call the same fake method exactly once.

- [ ] **Step 2: Run QML smoke test and confirm missing objects**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=QmlSmokeTest.EditorPageProvidesDurableEditControls`

Expected: failure to find `editorSplitButton`.

- [ ] **Step 3: Implement selection and controls**

Add a `TapHandler` to each clip delegate and bind selected border/colour to
controller identities. Buttons use controller capability properties for
`enabled`. Show exact marked duration and selected clip bounds in the Inspector.
Add `Shortcut` entries for Space, S, `[`, `]`, Delete, Shift+Delete, Ctrl+Z,
Ctrl+Shift+Z, and Ctrl+S; their handlers call the same controller methods as the
visible controls.

- [ ] **Step 4: Run all QML smoke tests**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=QmlSmokeTest.*`

Expected: all QML smoke tests pass offscreen.

- [ ] **Step 5: Commit**

```powershell
git add qml/EditorPage.qml tests/app/QmlSmokeTest.cpp
git commit -m "feat(editor): add split trim and ripple controls"
```

### Task 5: Application project-open wiring

**Files:**
- Modify: `src/main.cpp`
- Modify: `tests/app/ProjectControllerTest.cpp`
- Modify: `tests/app/EditorControllerTest.cpp`

**Interfaces:**
- Consumes `ProjectController::projectOpened` and `projectUrl()`.
- Calls `EditorController::openProject(projectController.projectUrl())` only after a successful validated project open or recovery.

- [ ] **Step 1: Add a failing integration test**

Create/open a real Unicode package through `ProjectController`, connect its
`projectOpened` signal to the editor exactly as `main.cpp` does, and assert the
editor publishes revision 0 and the default tracks without blocking the UI
event loop.

- [ ] **Step 2: Add the production connection**

```cpp
QObject::connect(&projectController, &ProjectController::projectOpened,
                 &editorController, [&] {
                     editorController.openProject(projectController.projectUrl());
                 });
```

Place the connection before QML load. Recovery continues to emit
`projectOpened`, so it follows the identical editor-open path.

- [ ] **Step 3: Run project/editor integration tests**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=ProjectControllerTest.*:ProjectEditorIntegrationTest.*`

Expected: all selected tests pass.

- [ ] **Step 4: Commit**

```powershell
git add src/main.cpp tests/app/ProjectControllerTest.cpp tests/app/EditorControllerTest.cpp
git commit -m "feat(app): open durable editor with project"
```

### Task 6: Physical reopen acceptance

**Files:**
- Create: `tests/acceptance/R1DurableEditAcceptanceTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Uses the real project package, SQLite timeline store, controller worker, fake edit engine, and QCoreApplication.

- [ ] **Step 1: Write the end-to-end acceptance**

Seed two video clips and one audio clip, then through controller invokables:
split clip A, trim the new right clip, lift a marked interval, undo, redo, ripple
delete another interval, and explicit save. Record the final snapshot and state.
Destroy every controller/worker/store, reopen from disk, and assert exact
timeline equality, revision, `clean`, `canUndo`, `canRedo`, and MLT load/update
revision sequence.

- [ ] **Step 2: Add injected transaction-failure acceptance**

Fail `commitEdit` during ripple delete and prove the model rows, selected clip,
playhead, revision, clean state, engine call count, and SQLite rows are unchanged.

- [ ] **Step 3: Run acceptance repeatedly**

Run: `build/windows-debug/cs_r1_durable_edit_acceptance_tests.exe --gtest_repeat=5`

Expected: 10/10 repeated acceptance cases pass with no stale callback or locked database.

- [ ] **Step 4: Commit**

```powershell
git add tests/acceptance/R1DurableEditAcceptanceTest.cpp tests/CMakeLists.txt
git commit -m "test(r1-04): prove durable edit reopen workflow"
```

### Task 7: Documentation, clean gate, and integration

**Files:**
- Create: `docs/superpowers/reports/2026-07-17-r1-04-verification.md`
- Modify: `IMPLEMENTATION_ROADMAP.md`

**Interfaces:**
- Records exact evidence only; it does not change product behavior.

- [ ] **Step 1: Run source checks and clean build**

Run `git diff --check`, then configure the audited MLT build and run a clean
`/W4 /permissive- /WX` build. Expected: zero warnings and every build step pass.

- [ ] **Step 2: Run complete sequential verification**

Run `ctest --test-dir build/windows-mlt-debug -j 1 --output-on-failure` with the
audited Qt/MLT runtime paths. Expected: 100 percent pass; no skip or disabled
test is accepted as evidence.

- [ ] **Step 3: Run application responsiveness and reopen checks**

Launch `creator_studio.exe`, confirm `Responding=True`, perform the physical
Unicode package reopen acceptance, and verify no test/fake library appears in
the shipping link command.

- [ ] **Step 4: Write the verification report and update roadmap**

Record clean-build steps, test totals/time, acceptance evidence, any platform
gap, and remaining R1-05 scope. Mark only R1-04 complete.

- [ ] **Step 5: Commit and review**

```powershell
git add docs/superpowers/reports/2026-07-17-r1-04-verification.md IMPLEMENTATION_ROADMAP.md
git commit -m "docs(r1-04): record durable edit verification"
```

Use `superpowers:requesting-code-review`, correct every confirmed finding with
tests, rerun the full gate, then merge `feat/r1-04-edit-operations` into
`feat/r1-usable-recorder-editor` without touching user-owned untracked files.
