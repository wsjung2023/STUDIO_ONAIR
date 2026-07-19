# R1-01 Timeline Domain and Durable Edit History Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the Qt-free R1 timeline source of truth, core split/trim/delete/ripple commands, SQLite migration 002, and atomic timeline/undo persistence that later Editor and MLT tasks consume.

**Architecture:** `cs_domain` owns typed, validated media/timeline values and reversible commands. `cs_project_store` owns the migration and converts domain snapshots to normalized SQLite rows in one transaction with the command log and undo cursor. No Qt, FFmpeg, MLT, JSON library, or filesystem handle crosses into the domain.

**Tech Stack:** C++20, `core::Result`, exact `ProjectClock` time types, SQLite 3.53.3, GoogleTest 1.15.2, CMake/Ninja, MSVC 2022.

## Global Constraints

- This plan is R1 delivery order item 1, not an MVP completion boundary; R1 remains incomplete until all eight master-design delivery items pass.
- `cs_domain` and `cs_project_store` stay Qt/FFmpeg/MLT-free.
- All persisted media time is signed 64-bit nanoseconds and all domain media time uses `TimestampNs`/`DurationNs`.
- Source media is immutable; timeline editing changes metadata only.
- Every mutation is a command; failed commands change neither the timeline nor history.
- Materialized timeline rows, command-log entry, undo cursor, and revision commit atomically.
- Existing migration 001 projects upgrade transactionally; source files are never touched by migration.
- Tests are written and observed failing before production code for every behavior.
- Work occurs on `feat/r1-01-timeline-domain` in `.worktrees/r1-01-timeline-domain`.
- The three untracked screenshots in the primary checkout are user files and remain untouched.

## Build commands used by every task

Run from `D:\Projects\STUDIO\.worktrees\r1-01-timeline-domain` in a VS 2022 x64 developer environment:

```powershell
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
$ninja = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$env:PATH = "C:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"
```

The initial configure command is:

```powershell
cmd /d /s /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 && "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build/r1-01-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 -DCMAKE_MAKE_PROGRAM="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe" -DCS_ENABLE_FFMPEG=OFF -DCS_WARNINGS_AS_ERRORS=ON'
```

---

### Task 1: Typed edit identities and validated time/value objects

**Files:**
- Modify: `src/domain/Identifiers.h`
- Create: `src/domain/TimelineTypes.h`
- Create: `src/domain/TimelineTypes.cpp`
- Modify: `src/domain/CMakeLists.txt`
- Create: `tests/domain/TimelineTypesTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `AssetId`, `TimelineId`, `TrackId`, `ClipId`, `SceneId`, `CommandId`, `RenderJobId`.
- Produces: `TimeRange::create(TimestampNs, DurationNs)`, `TimeRange::end()`, `overlaps(TimeRange, TimeRange)`.
- Produces: `VisualTransform::create(...)` and `AudioEnvelope::create(gainDb, fadeIn, fadeOut, clipDuration)`.

- [ ] **Step 1: Write failing value-object tests**

Add focused tests proving distinct typed IDs, positive non-overflowing ranges, half-open overlap semantics, normalized transform bounds, finite values, and fade sums no greater than clip duration. The intended API is:

```cpp
auto range = TimeRange::create(at(10s), 5s);
ASSERT_TRUE(range.hasValue());
EXPECT_EQ(range.value().end(), at(15s));
EXPECT_FALSE(overlaps(range.value(), TimeRange::create(at(15s), 1s).value()));

auto envelope = AudioEnvelope::create(-6.0, 1s, 2s, 10s);
ASSERT_TRUE(envelope.hasValue());
EXPECT_DOUBLE_EQ(envelope.value().gainDb(), -6.0);
```

- [ ] **Step 2: Configure/build and verify RED**

Run the initial configure, then:

```powershell
cmd /d /s /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 && "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build/r1-01-debug --target cs_tests'
```

Expected: compile failure because `TimelineTypes.h` and edit ID aliases do not exist.

- [ ] **Step 3: Implement the minimal value objects**

Use private constructors and `Result<T>` factories. `TimeRange` rejects negative start, non-positive duration, and `int64_t` overflow. `VisualTransform` stores normalized `x`, `y`, `width`, `height`, crop edges, opacity, finite rotation, and signed z-order; width/height must be positive and every normalized/crop value must be in `[0,1]`, with opposing crop sums below 1. `AudioEnvelope` accepts gain in `[-96,24]` dB, non-negative fades, and `fadeIn + fadeOut <= clipDuration` without overflowing.

Add the exact aliases:

```cpp
struct AssetIdTag;
struct TimelineIdTag;
struct TrackIdTag;
struct ClipIdTag;
struct SceneIdTag;
struct CommandIdTag;
struct RenderJobIdTag;
using AssetId = Identifier<AssetIdTag>;
using TimelineId = Identifier<TimelineIdTag>;
using TrackId = Identifier<TrackIdTag>;
using ClipId = Identifier<ClipIdTag>;
using SceneId = Identifier<SceneIdTag>;
using CommandId = Identifier<CommandIdTag>;
using RenderJobId = Identifier<RenderJobIdTag>;
```

- [ ] **Step 4: Verify GREEN**

Build `cs_tests`, then run:

```powershell
& $ctest --test-dir build/r1-01-debug -R 'IdentifiersTest|TimeRangeTest|VisualTransformTest|AudioEnvelopeTest' --output-on-failure
```

Expected: all selected tests pass with no warnings under `/WX`.

- [ ] **Step 5: Commit**

```powershell
git add src/domain tests/domain/TimelineTypesTest.cpp tests/CMakeLists.txt
git commit -m "feat(domain): add typed timeline values"
```

### Task 2: Immutable media assets and timeline aggregate

**Files:**
- Create: `src/domain/MediaAsset.h`
- Create: `src/domain/MediaAsset.cpp`
- Create: `src/domain/Timeline.h`
- Create: `src/domain/Timeline.cpp`
- Modify: `src/domain/CMakeLists.txt`
- Create: `tests/domain/MediaAssetTest.cpp`
- Create: `tests/domain/TimelineTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 IDs and value objects.
- Produces: `MediaAsset::create`, `Clip::create`, `Track::create`, `Timeline::create`.
- Produces: `Timeline::addTrack`, `insertClip`, `replaceClip`, `removeClip`, `track`, and immutable `tracks()`.

- [ ] **Step 1: Write failing aggregate tests**

Test video/audio asset metadata, rejection of empty/absolute/traversing package paths, positive duration, matching track/asset kinds, no duplicate IDs, positive clip ranges within the asset, and non-overlapping half-open clips. Exercise real aggregate behavior:

```cpp
auto timeline = Timeline::create(timelineId, "Main", frameRate).value();
ASSERT_TRUE(timeline.addTrack(Track::create(videoTrackId, TrackKind::Video,
                                           "Screen", true, false).value()).hasValue());
ASSERT_TRUE(timeline.insertClip(videoTrackId, screenClip).hasValue());
EXPECT_EQ(timeline.tracks().front().clips().front(), screenClip);
```

- [ ] **Step 2: Build and verify RED**

Expected: compile failure because `MediaAsset`, `Clip`, `Track`, and `Timeline` do not exist.

- [ ] **Step 3: Implement assets and aggregate**

Define:

```cpp
enum class MediaKind { Video, Audio, Image };
enum class AssetAvailability { Available, Offline };
enum class TrackKind { Video, Audio, Title, Caption };
enum class ClipKind { Asset, Title, Caption };
```

`MediaAsset` owns ID, kind, normalized portable relative path, duration, optional video/audio metadata, byte size, fingerprint, and availability. `Clip` owns ID, kind, optional asset ID, source/timeline ranges, enabled state, and optional visual/audio values. Initial asset clips require an asset ID; generated kinds forbid it. `Track` owns ordered clips. `Timeline` owns ordered tracks and exact frame rate.

All mutators validate into a temporary copy before committing so a failure leaves the aggregate byte-for-byte equivalent. Track order is vector order; clip order is `(timeline start, clip ID)`.

- [ ] **Step 4: Verify GREEN and regression scope**

```powershell
& $ctest --test-dir build/r1-01-debug -R 'MediaAssetTest|TimelineTest|RecordingSessionTest|ProjectManifestTest' --output-on-failure
```

Expected: all selected tests pass.

- [ ] **Step 5: Commit**

```powershell
git add src/domain tests/domain tests/CMakeLists.txt
git commit -m "feat(domain): add media timeline aggregate"
```

### Task 3: Split/trim commands and bounded edit history

**Files:**
- Create: `src/domain/EditCommand.h`
- Create: `src/domain/SplitClipCommand.h`
- Create: `src/domain/SplitClipCommand.cpp`
- Create: `src/domain/TrimClipCommand.h`
- Create: `src/domain/TrimClipCommand.cpp`
- Create: `src/domain/EditHistory.h`
- Create: `src/domain/EditHistory.cpp`
- Modify: `src/domain/CMakeLists.txt`
- Create: `tests/domain/EditCommandTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: mutable `Timeline` only through validated aggregate operations.
- Produces: `IEditCommand::execute/undo/clone`, `commandId`, `type`, `payload`, `undoPayload`.
- Produces: `EditHistory::execute`, `undo`, `redo`, `cursor`, `size`, `markClean`, `isClean`.

- [ ] **Step 1: Write failing command/history tests**

Prove split at an internal boundary creates two exact source/timeline ranges and undo restores the original ID/range. Prove leading/trailing trim moves the correct source edge. Reject boundary/outside splits and zero-duration trims. Prove a failed command is absent from history, new execution after undo removes the redo branch, and a configured maximum command count evicts only the oldest committed command.

- [ ] **Step 2: Build and verify RED**

Expected: compile failure because command/history headers do not exist.

- [ ] **Step 3: Implement commands and history**

Use this Qt-free interface:

```cpp
struct EditCommandRecord final {
    CommandId commandId;
    std::string type;
    std::string payload;
    std::string undoPayload;
};

class IEditCommand {
public:
    virtual core::Result<void> execute(Timeline&) = 0;
    virtual core::Result<void> undo(Timeline&) = 0;
    [[nodiscard]] virtual EditCommandRecord record() const = 0;
    [[nodiscard]] virtual std::unique_ptr<IEditCommand> clone() const = 0;
    virtual ~IEditCommand() = default;
};
```

Payload strings are canonical UTF-8 JSON text produced without a JSON dependency by fixed field formatting and JSON-string escaping helpers local to the command translation unit. They contain only IDs and integer nanoseconds. `EditHistory` owns `unique_ptr<IEditCommand>`, has an explicit constructor limit, and never silently grows without bound.
Its copy constructor deep-copies commands through `clone()` so an application
service can stage execute/undo/redo and discard the staged history on a failed
database transaction.

- [ ] **Step 4: Verify GREEN**

```powershell
& $ctest --test-dir build/r1-01-debug -R 'EditCommandTest|TimelineTest' --output-on-failure
```

Expected: all selected tests pass.

- [ ] **Step 5: Commit**

```powershell
git add src/domain tests/domain/EditCommandTest.cpp tests/CMakeLists.txt
git commit -m "feat(domain): add reversible split and trim edits"
```

### Task 4: Range delete and ripple delete

**Files:**
- Create: `src/domain/DeleteRangeCommand.h`
- Create: `src/domain/DeleteRangeCommand.cpp`
- Modify: `src/domain/CMakeLists.txt`
- Create: `tests/domain/DeleteRangeCommandTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Timeline`, an exact half-open deletion `TimeRange`, and `bool ripple`.
- Produces: atomic multi-track range deletion with exact undo/redo.

- [ ] **Step 1: Write failing range-deletion tests**

Cover clips outside the range, fully contained clips, left/right overlap, a clip spanning the whole deletion, multiple unlocked tracks, locked-track preservation, gap deletion, and ripple shifting all material at/after the deleted range left by its duration. Use explicit output IDs for any right-hand clip created by a spanning split so redo is deterministic.

- [ ] **Step 2: Build and verify RED**

Expected: compile failure because `DeleteRangeCommand` does not exist.

- [ ] **Step 3: Implement atomic deletion**

For each unlocked track, calculate a replacement clip vector without mutating the track. Validate every replacement vector. Commit all replacement vectors only after every track succeeds. Store only affected tracks' prior clip vectors for undo. A non-ripple delete preserves timeline coordinates; ripple closes exactly the requested range, including gaps. Locked tracks are neither cut nor shifted.

- [ ] **Step 4: Verify GREEN**

```powershell
& $ctest --test-dir build/r1-01-debug -R 'DeleteRangeCommandTest|EditCommandTest|TimelineTest' --output-on-failure
```

Expected: all selected tests pass.

- [ ] **Step 5: Commit**

```powershell
git add src/domain tests/domain/DeleteRangeCommandTest.cpp tests/CMakeLists.txt
git commit -m "feat(domain): add range and ripple deletion"
```

### Task 5: Transactional migration 002

**Files:**
- Create: `src/project_store/migrations/002_timeline_editing.sql`
- Create: `cmake/Migration002.h.in`
- Modify: `CMakeLists.txt`
- Modify: `src/project_store/CMakeLists.txt`
- Modify: `src/project_store/MigrationRunner.h`
- Modify: `src/project_store/MigrationRunner.cpp`
- Modify: `tests/project_store/MigrationRunnerTest.cpp`

**Interfaces:**
- Produces: checksum-pinned schema version 2 with media, timeline, edit history, checkpoint, title/caption, transform/envelope, and marker tables.

- [ ] **Step 1: Write failing migration tests**

Change the existing version expectation to 2. Assert a version-1 database upgrades without losing its project/session/segment rows, all migration-002 tables and indexes exist, foreign keys reject orphan clips, checks reject invalid time/kind pairs, applying twice is idempotent, a changed version-2 checksum is rejected, and an injected broken second migration rolls back only version 2.

- [ ] **Step 2: Run migration tests and verify RED**

```powershell
& $ctest --test-dir build/r1-01-debug -R MigrationRunnerTest --output-on-failure
```

Expected: failure because latest version is still 1 and the new tables are absent.

- [ ] **Step 3: Add the exact schema**

Add these tables with project/timeline foreign keys and explicit checks: `media_assets`, `timelines`, `tracks`, `clips`, `clip_visual_transforms`, `clip_audio_envelopes`, `titles`, `caption_cues`, `markers`, `edit_commands`, and `edit_checkpoints`. Enforce one primary timeline per project with a partial unique index; unique track positions per timeline; unique clip IDs; positive source/timeline durations; non-negative starts; asset/generated-kind consistency; valid command sequence/cursor/revision; and cascading deletion only for project-owned metadata.

Embed migration 002 exactly like 001 using its own SHA-256 at configure time, add it to the descriptor array, and set `kLatestVersion = 2`.

- [ ] **Step 4: Verify GREEN**

Run all `MigrationRunnerTest` cases. Expected: pass, including version-1 upgrade and rollback.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt cmake src/project_store tests/project_store/MigrationRunnerTest.cpp
git commit -m "feat(project-store): add timeline schema migration"
```

### Task 6: SQLite media and timeline snapshot store

**Files:**
- Create: `src/project_store/ITimelineStore.h`
- Create: `src/project_store/SqliteTimelineStore.h`
- Create: `src/project_store/SqliteTimelineStore.cpp`
- Modify: `src/project_store/CMakeLists.txt`
- Create: `tests/project_store/SqliteTimelineStoreTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `SqliteTimelineStore::open(databasePath, ProjectId)`.
- Produces: `putAsset`, `asset`, `assets`, `createTimeline`, `loadPrimaryTimeline`.
- Produces: `commitEdit(Timeline, EditCommandRecord, cursor, cleanCursor)` and
  `loadEditHistory()`.

- [ ] **Step 1: Write failing round-trip and validation tests**

Create a real temporary project database through `SqliteProjectDatabase::create`. Round-trip video/audio assets and a multi-track timeline containing transform and audio envelope values. Reopen through a second store and compare the full domain values. Test duplicate/missing IDs, project identity mismatch, malformed enum/time/ID rows inserted through a raw connection, and an injected SQLite constraint failure that leaves the prior revision unchanged.

- [ ] **Step 2: Build and verify RED**

Expected: compile failure because `SqliteTimelineStore` does not exist.

- [ ] **Step 3: Implement normalized persistence**

`SqliteTimelineStore` owns a separate WAL-enabled `SqliteConnection` and verifies exactly one matching project row. `putAsset` is immutable/idempotent: identical data succeeds, conflicting data returns `AlreadyExists`. `createTimeline` writes revision 0 and checkpoint cursor 0. `commitEdit` begins an immediate transaction, verifies `incoming revision == stored revision + 1`, replaces only that timeline's materialized child rows in foreign-key-safe order, appends the command record, updates revision/cursors, and commits. All persisted values are range-checked before converting to domain types.

Use the already-approved `nlohmann/json` only inside `cs_project_store` to parse
the command JSON emitted by domain commands. A private `EditCommandCodec` checks
the exact field set and types, then calls command-specific `rehydrate(...)`
factories with the original clip/track values required by undo. Unknown command
types or extra/missing fields return `ParseFailure`. `loadEditHistory()` reads
sequence order, decodes every command up to the bounded history limit, restores
the cursor/clean cursor, and rejects a cursor outside `[0, command_count]`.

- [ ] **Step 4: Verify GREEN**

```powershell
& $ctest --test-dir build/r1-01-debug -R 'SqliteTimelineStoreTest|SqliteProjectDatabaseTest|MigrationRunnerTest' --output-on-failure
```

Expected: all selected tests pass.

- [ ] **Step 5: Commit**

```powershell
git add src/project_store tests/project_store/SqliteTimelineStoreTest.cpp tests/CMakeLists.txt
git commit -m "feat(project-store): persist timeline snapshots"
```

### Task 7: Durable execute/undo/redo application service

**Files:**
- Create: `src/domain/TimelineRevision.h`
- Create: `src/app/TimelineEditService.h`
- Create: `src/app/TimelineEditService.cpp`
- Modify: `src/domain/CMakeLists.txt`
- Modify: `src/app/CMakeLists.txt`
- Create: `tests/app/TimelineEditServiceTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Timeline`, `EditHistory`, and `ITimelineStore`.
- Produces: synchronous Qt-free command core wrapped by the Qt application worker in later Editor tasks.
- Produces: `execute(unique_ptr<IEditCommand>)`, `undo()`, `redo()`, `markExplicitSave()`, `snapshot()`, `revision()`, `isClean()`.

- [ ] **Step 1: Write failing service tests**

Use a real `SqliteTimelineStore`, not a mock. Verify execute, reopen, undo, redo, branch-after-undo, explicit clean checkpoint, and revision monotonicity. Inject a store failure and prove the in-memory timeline/history roll back to their exact prior state. Recreate the service after every cursor position and verify the same snapshot and clean/dirty status.

- [ ] **Step 2: Build and verify RED**

Expected: compile failure because `TimelineEditService` does not exist.

- [ ] **Step 3: Implement durable service behavior**

The service deep-copies the aggregate/history state, applies execute/undo/redo to
that staged state, persists the resulting snapshot and cursor, then swaps the
staged state into memory only after commit. Undo and redo update the durable
cursor and append `UNDO`/`REDO` audit events referencing the affected command ID;
they do not duplicate the original edit command in history. Explicit save updates
the clean checkpoint in its own transaction without pretending to add an edit.
No database work is exposed to QML; later `EditorController` will call this
service on `ProjectWorker`.

- [ ] **Step 4: Verify GREEN**

```powershell
& $ctest --test-dir build/r1-01-debug -R 'TimelineEditServiceTest|SqliteTimelineStoreTest|EditCommandTest|DeleteRangeCommandTest' --output-on-failure
```

Expected: all selected tests pass.

- [ ] **Step 5: Commit**

```powershell
git add src/domain src/app tests/app/TimelineEditServiceTest.cpp tests/CMakeLists.txt
git commit -m "feat(app): make timeline edits durable"
```

### Task 8: R1-01 acceptance, documentation, and integration evidence

**Files:**
- Modify: `ARCHITECTURE.md`
- Modify: `IMPLEMENTATION_ROADMAP.md`
- Create: `docs/superpowers/reports/2026-07-17-r1-01-verification.md`

**Interfaces:**
- Consumes: all prior tasks.
- Produces: current-state documentation and reproducible verification evidence; no claim that full R1 is complete.

- [ ] **Step 1: Add an integration acceptance test before documentation**

Extend `TimelineEditServiceTest.cpp` with a real workflow: create a project, register screen/camera/microphone assets, create three tracks, insert clips, split a mistake, ripple-delete it across unlocked tracks, trim the camera, undo/redo, mark explicit save, close, reopen, and compare every timeline row plus cursor/revision. Build and observe the new test fail before completing any missing behavior.

- [ ] **Step 2: Make the acceptance test pass and run targeted tests**

Run all domain, migration, timeline-store, and service tests. Expected: pass with no warnings.

- [ ] **Step 3: Run the complete non-FFmpeg suite**

```powershell
$env:PATH = "C:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"
& $ctest --test-dir build/r1-01-debug --output-on-failure
```

Expected: all discovered tests pass. Record the exact count and elapsed time.

- [ ] **Step 4: Verify architectural and licensing boundaries**

Run:

```powershell
rg -n '#include <Q|#include "Q|FFmpeg|libav|mlt\+\+|<mlt' src/domain src/project_store
git diff --check
git status --short
```

Expected: no forbidden includes in Qt-free layers, no whitespace errors, and only intentional files changed. Migration 002 adds no new third-party dependency, so `legal/OSS_BOM.csv` remains unchanged and the report states that explicitly.

- [ ] **Step 5: Update docs and write verification report**

Document the implemented domain/persistence contracts and mark only R1 delivery-order item 1 as code-complete. The report lists commands, exact results, known platform boundary, and the next R1 task: Editor view models with a fake edit engine.

- [ ] **Step 6: Commit**

```powershell
git add ARCHITECTURE.md IMPLEMENTATION_ROADMAP.md docs/superpowers/reports tests/app/TimelineEditServiceTest.cpp
git commit -m "docs(r1-01): record timeline foundation verification"
```

## Plan self-review result

- Spec coverage: covers master-design delivery item 1 only and explicitly preserves the remaining R1 scope.
- Placeholder scan: every implementation step names concrete behavior, files,
  commands, and expected evidence.
- Type consistency: all later tasks consume the IDs, value objects, aggregate, command record, store, and service names introduced earlier.
- Isolation: domain, persistence, and application responsibilities remain separate; SQLite and Qt do not leak into domain types.
- Verification: each production behavior has an observed RED step, targeted GREEN step, regression scope, and final full-suite gate.
