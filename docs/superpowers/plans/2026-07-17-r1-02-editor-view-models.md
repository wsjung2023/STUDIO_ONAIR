# R1-02 Edit Engine Port and Editor View Models Plan

> Scope: R1 delivery-order item 2. This is a checkpoint inside the full R1
> recorder/editor/export product, not an MVP boundary.

## Goal

Introduce the stable Qt-free edit-engine port, a deterministic fake engine, and
real media-bin/multi-track Qt models. Publish them through an asynchronous
Editor controller so the next slice can add the audited MLT adapter without
changing product truth or UI contracts.

## Architecture decisions

- `domain::Timeline` plus `TimelineRevision` remains product truth.
- `edit_engine` is a Qt-free port module. It exposes immutable snapshots,
  revision-checked change sets, preview frames, render requests/jobs, and
  playback commands without MLT types.
- `FakeEditEngine` is test-only production-quality test infrastructure in
  `cs_fakes`; it records calls, enforces state, supports deterministic failures,
  and never enters the shipped application target.
- Media bin and timeline tracks use `QAbstractListModel`, not anonymous
  `QVariantList` snapshots. Stable roles let QML render large projects without
  replacing controller APIs later.
- Engine calls run on `EditorEngineWorker` in a dedicated `QThread`. The UI
  thread owns models and state only.
- A durable edit is never rolled back because engine synchronization fails.
  The controller marks preview stale and requests a full graph rebuild.
- MLT remains a future dynamic LGPL adapter with a pinned module allowlist;
  this slice introduces no new third-party dependency.

## Task 1: Define the Qt-free edit-engine contract

**Files**

- Create `src/edit_engine/CMakeLists.txt`
- Create `src/edit_engine/EditEngineTypes.h/.cpp`
- Create `src/edit_engine/IEditEngine.h`
- Modify root `CMakeLists.txt`
- Create `tests/edit_engine/EditEngineTypesTest.cpp`
- Modify `tests/CMakeLists.txt`

**RED tests**

- Reject negative/out-of-order revisions and duplicate/unbounded changed-track
  lists.
- Reject preview frames whose requested position differs from the returned
  product position or whose revision is stale.
- Reject unsafe render destinations, zero dimensions/bitrates, and invalid
  progress/state transitions.

**Implementation**

- `TimelineSnapshot`: timeline plus revision.
- `TimelineChangeSet`: expected base revision, immutable target snapshot,
  unique affected track IDs, and explicit rebuild flag.
- `PreviewFrame`: product position, revision, neutral `media::VideoFrame`.
- Validated `RenderPreset`, `RenderRequest`, `RenderProgress`, `IRenderJob`.
- `IEditEngine`: load/update/play/pause/seek/requestFrame/render exactly at the
  domain boundary defined by the R1 design.

## Task 2: Build a deterministic fake engine

**Files**

- Create `src/fakes/FakeEditEngine.h/.cpp`
- Modify `src/fakes/CMakeLists.txt`
- Create `tests/fakes/FakeEditEngineTest.cpp`

**RED tests**

- Reject playback, seek, update, frame, and render before load.
- Record load/update/play/pause/seek calls in order.
- Apply only revision-compatible change sets.
- Return deterministic neutral preview frames.
- Inject a one-shot failure per operation without corrupting fake state.
- Return a cancellable deterministic render job.

## Task 3: Implement media-bin and multi-track models

**Files**

- Create `src/app/MediaBinModel.h/.cpp`
- Create `src/app/TimelineTrackModel.h/.cpp`
- Modify `src/app/CMakeLists.txt`
- Create `tests/app/EditorModelsTest.cpp`

**RED tests**

- Media roles expose stable asset identity, package path, type, duration,
  availability, and stream metadata.
- Track roles expose stable order, identity, type, enabled/locked state, and
  clip DTOs with source/timeline ranges, transform, and audio envelope.
- Model reset emits one correct reset and leaves no stale indexes.
- Unicode names and offline assets survive conversion exactly.

## Task 4: Add the asynchronous Editor controller

**Files**

- Create `src/app/EditorEngineWorker.h/.cpp`
- Create `src/app/EditorController.h/.cpp`
- Modify `src/app/CMakeLists.txt`
- Create `tests/app/EditorControllerTest.cpp`

**RED tests**

- Opening a session publishes models on the UI thread and loads the fake engine
  on its worker thread.
- Play, pause, and seek are serialized; stale callbacks cannot overwrite a
  newer session.
- A committed timeline update refreshes models first, then updates the engine.
- Engine update failure preserves the durable snapshot, marks preview stale,
  and schedules a full reload.
- Destruction drains or cancels pending work exactly once.

## Task 5: Replace the static Editor shell with model-driven QML

**Files**

- Modify `src/main.cpp`
- Modify `qml/EditorPage.qml`
- Modify `qml/Main.qml`
- Extend `tests/app/QmlSmokeTest.cpp`

**RED tests**

- EditorPage requires the controller contract and loads with empty models.
- Media-bin delegates show asset name/type/offline state.
- Timeline delegates show real track names and clip geometry from typed
  nanosecond ranges.
- Busy, stale-preview, play/pause, and error states are visible.

The shipping application uses an explicit unavailable engine until the audited
MLT adapter lands; it never links the fake.

## Task 6: Acceptance, review, and integration

**Files**

- Modify `ARCHITECTURE.md`
- Modify `IMPLEMENTATION_ROADMAP.md`
- Create `docs/superpowers/reports/2026-07-17-r1-02-verification.md`

**Acceptance workflow**

Create screen/camera/microphone assets and a multitrack timeline; open it in the
Editor controller; verify media/track models; load and seek the fake engine;
commit a split; observe models update before the engine; inject update failure;
verify durable state remains and full reload clears stale preview.

**Final gates**

```powershell
cmake --build --preset windows-debug --clean-first
$env:QT_QPA_PLATFORM='offscreen'
ctest --preset windows-debug
rg -n '^[ \t]*#include' -- src/domain src/project_store src/edit_engine
git diff --check
```

Run independent code review, fix all actionable findings, rerun the complete
suite, merge into `feat/r1-usable-recorder-editor`, then continue to R1 delivery
item 3 (audited MLT bootstrap and real playback).
