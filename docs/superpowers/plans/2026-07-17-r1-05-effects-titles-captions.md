# R1-05 Effects, Titles, and Basic Captions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship durable, undoable visual transforms/PIP, audio gain/fades, titles, and basic caption cues whose real MLT preview and exact `.cstudio` reopen agree.

**Architecture:** Extend the Qt-free timeline and canonical command history as the sole source of truth, persist the already-provisioned migration-002 tables in the existing SQLite transaction, derive content-addressed Qt text rasters on the editor session thread, and compile asset/title/caption overlays plus Creator Studio-owned image/audio processing into the audited MLT core/avformat graph. The controller publishes durable state before derived cache/engine synchronization and QML remains a typed command surface.

**Tech Stack:** C++20, Qt 6 Core/Gui/Quick/QML, SQLite, MLT 7.40.0 audited `core` and `avformat` modules, GoogleTest, CMake/Ninja, MSVC `/W4 /permissive- /WX`.

## Global Constraints

- Preserve the approved full R1-R4 scope; this is R1 delivery item 5, not an MVP reduction.
- Follow red-green-refactor for every behavior: add one focused failing test, observe the expected failure, implement the minimum complete product behavior, then run the containing suite.
- Keep Qt out of `domain`, `project_store`, `edit_engine`, and the public MLT graph-plan contracts.
- Do not modify migration 002 or its checksum. Its transform, envelope, title, and caption tables are the schema contract.
- Never enable MLT `plus`, `qt6`, `normalize`, or another unaudited runtime module. No `melt` subprocess or MLT XML becomes project state.
- SQLite, cache rasterization/I/O, and MLT graph work remain off the UI thread.
- A database commit failure changes no domain, model, cache descriptor, history, or preview graph.
- A post-commit cache/engine failure preserves and publishes the committed edit, reports a visible entity-specific diagnostic, marks preview stale, and retries from the committed snapshot.
- Generated overlay paths are package-relative, validated below `cache/generated`, and derived from canonical SHA-256 keys. Completed content-addressed PNGs are not deleted in R1-05.
- Every new JSON decoder rejects unknown fields, malformed UTF-8/value bounds, contradictory undo data, and unsupported command versions.
- Source, canvas, clip, cue, and sample arithmetic is overflow-checked. No unchecked narrowing reaches SQLite, Qt, or MLT.
- Use `build/windows-mlt-debug` and the audited prefix at `../r1-03-mlt-preview/build/mlt/prefix`; no reduced non-MLT build may serve as final evidence.

---

### Task 1: Generated visual values, PIP presets, and complete clip invariants

**Files:**
- Modify: `src/domain/Identifiers.h`
- Modify: `src/domain/TimelineTypes.h`
- Modify: `src/domain/TimelineTypes.cpp`
- Modify: `src/domain/Timeline.h`
- Modify: `src/domain/Timeline.cpp`
- Test: `tests/domain/IdentifiersTest.cpp`
- Test: `tests/domain/TimelineTypesTest.cpp`
- Test: `tests/domain/TimelineTest.cpp`

**Interfaces:**
- Produces `CueId`, `TextAlignment`, `RgbaColor`, `TitlePayload`, `CaptionCue`, and `PipPreset`.
- Produces `visualTransformForPipPreset(...)` and `identifyPipPreset(...)` using source/canvas aspect ratios.
- Produces `Clip::createTitle`, `Clip::createCaption`, immutable `withVisualTransform`, `withAudioEnvelope`, `withTitlePayload`, and `withCaptionCues`.
- Produces validated `Timeline::removeTrack` for exact auto-created-track undo.

- [ ] **Step 1: Add failing value-boundary and preset tests**

Cover empty/overlong Unicode text by decoded code-point count, malformed UTF-8, invalid font family, non-canonical RGBA strings, normalized title placement, invalid cue ranges, deterministic cue ordering, overlap rejection, PIP safe margins/aspect preservation, exact preset recognition, and one-value manual edits returning `Custom`.

```cpp
auto preset = visualTransformForPipPreset(
    PipPreset::BottomRight, 16.0 / 9.0, 16.0 / 9.0, 7);
ASSERT_TRUE(preset.hasValue());
EXPECT_EQ(identifyPipPreset(preset.value(), 16.0 / 9.0, 16.0 / 9.0),
          PipPreset::BottomRight);
```

- [ ] **Step 2: Run and observe the missing-type failures**

Build `cs_tests`, then run:

`cs_tests.exe --gtest_filter=IdentifiersTest.*:TimelineTypesTest.*:TimelineTest.*`

Expected red state: compile errors for `CueId`, `RgbaColor`, and the generated clip constructors before implementation.

- [ ] **Step 3: Implement canonical values and transforms**

Add `CueIdTag` beside the existing typed identifiers. Store RGBA as four bytes and serialize only lowercase `#rrggbbaa`. Accept finite transform values only; preserve current -96 dB to +24 dB and non-overlapping fade rules. PIP width is exactly `0.30`, height is derived from `canvasAspect / sourceAspect`, and the four corner presets use exactly `0.04` margins.

- [ ] **Step 4: Implement mutually exclusive clip payloads and track removal**

Asset clips retain asset/media metadata. Title clips own one `TitlePayload`, caption clips own a non-empty sorted `std::vector<CaptionCue>`, and generated clips have no asset identity. Their source range starts at zero and equals the timeline duration. Reject title payload on captions, cues on titles, visual transforms on audio-only assets, and envelopes on clips without audio. `removeTrack` rejects missing/locked/non-empty tracks unless the caller supplies the exact expected empty generated track.

- [ ] **Step 5: Run the complete domain suite and commit**

Run all three focused suites, then `ctest -R "(Identifiers|TimelineTypes|TimelineTest)" -j1`.

Commit: `feat(domain): model effects titles and captions`

### Task 2: Durable effect and generated-content commands

**Files:**
- Create: `src/domain/SetVisualTransformCommand.h`
- Create: `src/domain/SetVisualTransformCommand.cpp`
- Create: `src/domain/SetAudioEnvelopeCommand.h`
- Create: `src/domain/SetAudioEnvelopeCommand.cpp`
- Create: `src/domain/GeneratedClipCommands.h`
- Create: `src/domain/GeneratedClipCommands.cpp`
- Modify: `src/domain/EditCommandJson.h`
- Modify: `src/domain/EditCommandJson.cpp`
- Modify: `src/domain/CMakeLists.txt`
- Test: `tests/domain/EditCommandTest.cpp`
- Create: `tests/domain/GeneratedEditCommandTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces `SetVisualTransformCommand` and `SetAudioEnvelopeCommand` with exact previous optional values.
- Produces `AddTitleCommand`, `EditTitleCommand`, `RemoveGeneratedClipCommand`, `AddCaptionCueCommand`, `EditCaptionCueCommand`, and `RemoveCaptionCueCommand`.
- Each command implements `execute`, `undo`, `clone`, `record`, and bounded `rehydrate` through the existing `IEditCommand` contract.

- [ ] **Step 1: Write failing execute/undo/redo tests for property commands**

Prove byte-equivalent timeline round trips for setting, replacing, and resetting transforms/envelopes; stale/missing IDs, incompatible kinds, locked tracks, and invalid envelope duration must fail without mutation.

- [ ] **Step 2: Implement property commands and run focused green tests**

Use immutable clip replacement through `Timeline::replaceClip`. Store only the prior optional property plus the requested new property. Canonical payload order is fixed by `EditCommandJson`; no hand-built locale-sensitive floating-point strings.

- [ ] **Step 3: Write failing generated-track/clip command tests**

Cover stable `title-1`/`caption-1` creation, reuse of a pre-existing track, undo removing only a track created by that command, caption cue sorting, overlap rejection, last-cue removal semantics, title/caption removal and undo, Unicode, and redo after reopen-style cloning.

- [ ] **Step 4: Implement generated commands atomically in staged timeline values**

Commands first validate every ID/value against a copied timeline, then publish the replacement. `AddTitleCommand`/first caption command record whether they created the stable track. `RemoveGeneratedClipCommand` is valid only for title/caption clips. Removing the last cue removes the caption clip but retains a pre-existing track; undo reconstructs both exactly.

- [ ] **Step 5: Add canonical records and rehydration**

Records include explicit command type/version, exact target identities, requested new values, previous values, created-track flag/metadata, removed clip, and previous cue where applicable. Decode every scalar through checked helpers and reject extra keys or payload/undo contradictions.

- [ ] **Step 6: Run domain/history suites and commit**

Run:

`cs_tests.exe --gtest_filter=EditCommandTest.*:GeneratedEditCommandTest.*:DeleteRangeCommandTest.*`

Commit: `feat(domain): add durable effect and text commands`

### Task 3: SQLite generated-clip persistence and history rehydration

**Files:**
- Modify: `src/project_store/SqliteTimelineStore.cpp`
- Modify: `src/project_store/internal/EditCommandCodec.cpp`
- Test: `tests/project_store/SqliteTimelineStoreTest.cpp`
- Test: `tests/project_store/RecoveryTest.cpp`

**Interfaces:**
- Extends `writeSnapshot` and `loadPrimaryTimeline` for `ASSET`, `TITLE`, and `CAPTION` clips.
- Extends `EditCommandCodec::decode` for every Task 2 command without changing the schema.

- [ ] **Step 1: Add a failing real-database Unicode round-trip test**

Create video/audio/title/caption tracks containing transforms, envelopes, a Korean title, and multiple Korean caption cues. Reopen through a new `SqliteTimelineStore` and compare the whole timeline, revision, clean cursor, history cursor, and command records.

- [ ] **Step 2: Add failing corruption and atomicity tests**

Inject partial title rows, wrong clip kind, invalid RGBA/alignment, caption cue beyond its clip, overlapping cues, orphan payloads, unknown command keys, contradictory undo state, and an SQLite trigger failure. Assert `CorruptData`/`UnsupportedVersion` as appropriate and no partial snapshot/event/checkpoint write.

- [ ] **Step 3: Implement kind-specific snapshot writing**

Bind nullable `asset_id`/`media_kind` for generated clips; insert `titles` and `caption_cues` from validated domain values. Continue inserting visual/audio rows only for compatible clips. Keep track deletion plus complete snapshot replacement inside the existing caller transaction.

- [ ] **Step 4: Implement fail-closed generated loading**

Branch on `clip_kind`, enforce nullability and matching track kind, load exactly one title row or one-or-more ordered caption rows, reject extra/missing payload rows, then call the Task 1 constructors. Restore the persisted locked state only after inserting validated clips, as existing asset loading does.

- [ ] **Step 5: Implement command decoder cases and run store/recovery suites**

Run:

`cs_tests.exe --gtest_filter=SqliteTimelineStoreTest.*:RecoveryTest.*:CrashRecoveryIntegrationTest.*`

Commit: `feat(project-store): persist effects titles and captions`

### Task 4: Session requests, canvas-aware snapshots, and durable publication boundary

**Files:**
- Modify: `src/edit_engine/EditEngineTypes.h`
- Modify: `src/edit_engine/EditEngineTypes.cpp`
- Modify: `src/app/EditorSessionTypes.h`
- Modify: `src/app/EditorSessionWorker.h`
- Modify: `src/app/EditorSessionWorker.cpp`
- Modify: `src/app/TimelineEditService.h`
- Modify: `src/app/TimelineEditService.cpp`
- Test: `tests/edit_engine/EditEngineTypesTest.cpp`
- Test: `tests/app/TimelineEditServiceTest.cpp`
- Test: `tests/app/EditorSessionWorkerTest.cpp`

**Interfaces:**
- Adds `canvasWidth`, `canvasHeight`, and `std::vector<GeneratedOverlayDescriptor>` to `TimelineSnapshot`.
- `GeneratedOverlayDescriptor` identifies clip, optional cue, package-relative raster path, timeline range, and resolved font family without Qt types.
- Extends `EditorEditKind` and `EditorEditRequest` with typed transform/envelope/title/caption operations and all generated identities supplied by the worker, not QML.
- Adds an optional post-commit derived-work diagnostic to `EditorSessionUpdate`.

- [ ] **Step 1: Add failing snapshot and worker request tests**

Prove manifest canvas dimensions survive project open; each new request creates the intended command; invalid/stale selection and locked tracks issue no commit; command/clip/cue IDs are generated in the worker; and one edit increments the durable revision exactly once.

- [ ] **Step 2: Extend snapshot validation and equality**

Canvas dimensions use the existing manifest bounds. Descriptor paths must be relative, normalized, non-empty, contain no root/parent traversal, and begin `cache/generated/`. Descriptor ranges must stay inside the owning clip/cue.

- [ ] **Step 3: Map every request to the Task 2 command set**

Preserve the R1-04 staged execute→single `commitEdit`→publish order. Mark the affected track for transform/envelope operations and require a full graph rebuild for generated track/clip changes, undo, and redo.

- [ ] **Step 4: Prove failure boundaries**

With injected `ITimelineStore`, prove commit failure returns the byte-identical previous session. Separately inject a derived-work failure result and prove the new revision/timeline is published with a diagnostic rather than reported as rolled back.

- [ ] **Step 5: Run app/service/edit-engine suites and commit**

Run:

`cs_app_tests.exe --gtest_filter=EditorSessionWorkerTest.*`

`cs_tests.exe --gtest_filter=TimelineEditServiceTest.*:EditEngineTypesTest.*`

Commit: `feat(editor): route durable effect and text edits`

### Task 5: Deterministic generated-overlay cache on the session thread

**Files:**
- Create: `src/app/GeneratedOverlayCache.h`
- Create: `src/app/GeneratedOverlayCache.cpp`
- Modify: `src/app/EditorSessionWorker.h`
- Modify: `src/app/EditorSessionWorker.cpp`
- Modify: `src/app/CMakeLists.txt`
- Create: `tests/app/GeneratedOverlayCacheTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces `GeneratedOverlayCache::synchronize(packageRoot, timeline, canvasWidth, canvasHeight, frameRate)`.
- Returns descriptors plus diagnostics and resolved font families; database/domain state is input-only.
- Uses `QImage::Format_ARGB32_Premultiplied`, `QTextLayout`, `QPainter`, `QCryptographicHash`, and atomic `QSaveFile` writes.

- [ ] **Step 1: Write failing canonical-key and raster tests**

Verify the SHA-256 key changes for identity, cue timing, text/style, canvas, frame rate, or resolved font; identical inputs reuse one path; Korean/Latin output has transparent exterior and non-transparent glyph pixels; two fresh renders are pixel-identical.

- [ ] **Step 2: Write failing recovery/safety tests**

Cover missing file, corrupt/non-PNG file, zero-size file, abandoned `.tmp` removal, symlink/reparse/path escape refusal, atomic replace failure, and unavailable requested font fallback. Completed unrelated hash PNGs must remain untouched.

- [ ] **Step 3: Implement canonical payload and atomic rasterization**

Build the cache key from fixed-order UTF-8 fields with length prefixes, integer RGBA, exact dimensions/frame-rate integers, and resolved family. Render title/caption backgrounds and aligned text with bounded layouts. Validate the final canonical path beneath the package before every read/write.

- [ ] **Step 4: Integrate after durable commit and on open**

Run synchronization inside `EditorSessionWorker`'s thread after `currentState` is durable. On success attach descriptors to the returned snapshot. On failure return the committed/opened snapshot with missing descriptors plus the diagnostic; reopening retries generation.

- [ ] **Step 5: Prove UI-thread responsiveness and commit**

Use a deliberately large raster fixture while a UI-thread timer continues firing. Run:

`cs_app_tests.exe --gtest_filter=GeneratedOverlayCacheTest.*:EditorSessionWorkerTest.*`

Commit: `feat(editor): derive deterministic text overlay cache`

### Task 6: Overlay graph planning and Creator Studio image/audio processors

**Files:**
- Modify: `src/mlt_adapter/MltGraphPlan.h`
- Modify: `src/mlt_adapter/MltGraphPlan.cpp`
- Create: `src/mlt_adapter/FrameEffects.h`
- Create: `src/mlt_adapter/FrameEffects.cpp`
- Modify: `src/mlt_adapter/CMakeLists.txt`
- Modify: `tests/mlt_adapter/MltGraphPlanTest.cpp`
- Create: `tests/mlt_adapter/FrameEffectsTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Graph plan separates audio track playlists from deterministic visual overlay branches.
- Each visual branch carries asset/generated path, clip/cue identity, range, transform, and order key `(zOrder, trackPosition, timelineStart, identity)`.
- Produces Qt/MLT-independent inverse-affine BGRA processing and float PCM envelope helpers consumed by the physical adapter.

- [ ] **Step 1: Add failing graph-plan tests**

Cover canvas propagation, per-clip branches from one domain video track, title and time-sliced caption descriptors, exact deterministic ordering, disabled/offline/missing-cache behavior, invalid descriptor ownership, and duration calculation.

- [ ] **Step 2: Implement graph-plan compilation**

Retain one playlist per enabled audio domain track. Expand every visual asset clip and every generated descriptor to an overlay branch. Asset visuals use the clip transform or identity; generated visuals use identity over their transparent full-canvas raster. Missing derived overlays remain explicit unavailable transparent branches plus diagnostics.

- [ ] **Step 3: Add failing pixel processor tests**

Use tiny asymmetric BGRA fixtures to prove identity bypass, translation/scale, every crop edge, 90-degree and non-right-angle inverse rotation, bilinear interpolation, opacity, transparent padding, bounds, overflow, and deterministic output.

- [ ] **Step 4: Add failing sample processor tests**

Use known float blocks to prove -6 dB/+6 dB gain, exact fade endpoints, a block crossing each fade boundary, interleaved channels, clip-local offsets, finite clamping, and no out-of-bounds sentinel changes.

- [ ] **Step 5: Implement bounded pure processors and commit**

Identity image input returns a no-copy view/flag. Other paths allocate exactly checked `canvasWidth * canvasHeight * 4` bytes and inverse-map destination pixels. Audio converts dB with `pow(10, db/20)`, computes gain per frame/sample position, clamps to `[-1,1]`, and never treats channels as timeline samples.

Run:

`cs_tests.exe --gtest_filter=MltGraphPlanTest.*:FrameEffectsTest.*`

Commit: `feat(mlt): plan overlays and process frame effects`

### Task 7: Physical audited-MLT transforms, envelopes, titles, and captions

**Files:**
- Modify: `src/mlt_adapter/MltEditEngine.cpp`
- Modify: `src/mlt_adapter/MltEditEngine.h`
- Modify: `tests/mlt_adapter/MltEditEngineTest.cpp`
- Modify: `tests/acceptance/R1MltPreviewAcceptanceTest.cpp`

**Interfaces:**
- Attaches Creator Studio-owned frame callbacks around audited avformat producers without registering or staging a new MLT module.
- Extends diagnostics with visual branch, transformed branch, envelope branch, and missing-overlay counts.
- Uses the same graph construction path for `load` and `update`.

- [ ] **Step 1: Write failing real-frame tests**

Generate deterministic fixtures and sample output pixels proving PIP position/size, crop, opacity, rotation, z-order between two clips on one domain track, Korean title visibility, caption visibility only inside each cue, and transparent behavior for a missing cache image.

- [ ] **Step 2: Write failing real-PCM tests**

Load a deterministic tone, request mixed float audio before/during/after fades, and compare measured amplitudes to expected gain/fade multipliers with a documented tolerance and exact sample windows.

- [ ] **Step 3: Build visual overlay branches**

Create one held avformat image/asset producer and playlist per plan branch, attach the Creator callback to obtain/process BGRA frames, and composite branches over the black background in the plan's exact order using only audited core `composite`.

- [ ] **Step 4: Attach audio envelope processing**

Keep audited `audioconvert` and `mix`; attach the Creator callback to each affected producer after float PCM is requested. Derive clip-local samples from frame position and the negotiated frequency using checked arithmetic.

- [ ] **Step 5: Verify lifecycle, update rollback, and runtime boundary**

Repeat graph construction/destruction, fail one update, and prove the last good graph remains. Re-run `MltRuntimeManifestTest.*` and `MltBootstrapPolicy`; the physical staged file set and hashes must remain unchanged.

- [ ] **Step 6: Run physical MLT suites and commit**

Run:

`cs_mlt_tests.exe --gtest_filter=MltEditEngineTest.*:MltRuntimeManifestTest.*`

`cs_r1_mlt_acceptance_tests.exe --gtest_repeat=3`

Commit: `feat(mlt): render durable effects titles and captions`

### Task 8: Typed controller inspector contract and failure recovery

**Files:**
- Modify: `src/app/EditorController.h`
- Modify: `src/app/EditorController.cpp`
- Modify: `src/app/TimelineTrackModel.h`
- Modify: `src/app/TimelineTrackModel.cpp`
- Modify: `tests/app/EditorModelsTest.cpp`
- Modify: `tests/app/EditorControllerTest.cpp`

**Interfaces:**
- Exposes selected clip/track compatibility and current transform/envelope/title/caption values as typed QML properties/model roles.
- Adds explicit-submit invokables for complete transform, five PIP presets/reset, complete envelope/reset, add/edit/remove title, and add/edit/remove caption cue.
- Never accepts generated identities, paths, or raw JSON from QML.

- [ ] **Step 1: Write failing model/controller contract tests**

Cover asset/title/caption roles, compatible/incompatible selection, current/custom PIP state, finite numeric parsing, canonical color parsing, Unicode bounds, cue overlap, stale selection, one durable request in flight, and explicit submission rather than per-keystroke edits.

- [ ] **Step 2: Implement typed model roles and selected-value properties**

Roles identify `clipKind`, compatible visual/audio controls, transform/envelope values, title payload, and caption cue rows. Controller getters derive from the current committed snapshot only and reset synchronously on project open or invalidated selection.

- [ ] **Step 3: Implement invokable validation and request mapping**

Convert all `QString`/numeric inputs into Task 1 domain values on the UI thread only for lightweight validation; post the typed request to the session worker. Disable duplicate submission while `sessionBusy`. Visible error text identifies the invalid field/entity.

- [ ] **Step 4: Implement post-commit derived failure behavior**

Publish new model/revision/undo state first. If the session update carries a cache diagnostic or the MLT update fails, preserve the edit, set `previewStale`, surface the diagnostic, and queue one generation-safe full rebuild. Ignore old cache/engine results.

- [ ] **Step 5: Run controller/model suites and commit**

Run:

`cs_app_tests.exe --gtest_filter=EditorModelsTest.*:EditorControllerTest.*`

Commit: `feat(editor): expose effects and text inspector contract`

### Task 9: Full Inspector UI, accessibility, and shortcut/control parity

**Files:**
- Modify: `qml/EditorPage.qml`
- Modify: `tests/app/QmlSmokeTest.cpp`

**Interfaces:**
- Adds stable object names for all Visual, Audio, Title, and Captions controls.
- Visible controls and keyboard actions call the same Task 8 invokables.

- [ ] **Step 1: Write failing offscreen QML tests**

Require visual fields/buttons, five PIP preset buttons, audio fields/reset, title add/edit/remove controls, caption cue list/add/edit/remove controls, explicit Apply buttons, accessibility names, TITLE/CAPTION timeline labels, Unicode rendering, busy/selection disabled states, and shortcut parity.

- [ ] **Step 2: Implement model-driven Inspector sections**

Use bounded text/numeric fields but submit only on Apply. Display the resolved/custom PIP state and resolved font diagnostic. Hide or disable incompatible sections without losing selected committed values. Generated clip delegates remain selectable and visually distinct.

- [ ] **Step 3: Add keyboard parity without conflicting with R1-04 editing**

Use one action function per operation and call it from both button and shortcut. Preserve Space/S/[ ]/Delete/Shift+Delete/Ctrl+Z/Ctrl+Shift+Z/Ctrl+S behavior. Text fields must consume editing keystrokes so global shortcuts do not corrupt typed content.

- [ ] **Step 4: Run all QML smoke tests and commit**

Run:

`cs_app_tests.exe --gtest_filter=QmlSmokeTest.*`

Commit: `feat(editor-ui): add effects title and caption inspector`

### Task 10: Durable physical acceptance, clean gate, review, and integration

**Files:**
- Create: `tests/acceptance/R1EffectsTextAcceptanceTest.cpp`
- Modify: `tests/CMakeLists.txt`
- Create: `docs/superpowers/reports/2026-07-17-r1-05-verification.md`
- Modify: `IMPLEMENTATION_ROADMAP.md`

**Interfaces:**
- Produces one dedicated physical acceptance executable using a real Unicode package, SQLite, generated cache, controller/worker, and audited MLT engine.
- Records exact reproducible evidence and marks only R1-05 complete.

- [x] **Step 1: Implement the real reopen workflow**

Through controller invokables set camera PIP with manual rotation/crop/opacity, set microphone gain/fades, add a Korean title and multiple Korean caption cues, undo/redo across effect/text commands, explicitly save, record pixels/PCM/domain state, destroy every worker/engine/store, reopen, and compare exact state plus measured preview pixels/audio samples.

- [x] **Step 2: Implement distinct injected failure workflows**

An SQLite commit failure must leave revision/model/history/cache/engine unchanged. A cache write failure after commit must advance durable state, publish a diagnostic/stale preview, and regenerate successfully after reopen. Repeat both to catch locked files and stale callbacks.

- [x] **Step 3: Measure the correctness-first graph**

Build a 30-minute representative timeline and record graph build time, frame request time, native branch/transition counts, memory, and responsiveness. If it exceeds the established R1 preview budget, implement deterministic non-overlapping visual lane packing here and repeat; do not defer a known R1 resource failure.

- [x] **Step 4: Run source and focused acceptance gates**

Run `git diff --check`, scan for TODO/FIXME/placeholders and permissive assertions, repeat the new physical acceptance five times, and run MLT manifest/bootstrap policy tests. No skipped/disabled test counts as evidence.

- [x] **Step 5: Run a fresh audited warning-clean build**

Delete only the verified R1-05 audit directory `build/windows-mlt-audit`, then run this exact configure/build sequence from the R1-05 worktree:

```powershell
& $env:ComSpec /d /c 'C:\PROGRA~2\MICROS~2\2022\BUILDT~1\VC\Auxiliary\Build\vcvars64.bat >nul && C:\PROGRA~2\MICROS~2\2022\BUILDT~1\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe -S . -B build\windows-mlt-audit -G Ninja -DCMAKE_MAKE_PROGRAM=D:\Projects\STUDIO\build\tools\vcpkg\downloads\tools\ninja-1.13.2-windows\ninja.exe -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64 -DCS_BUILD_TESTS=ON -DCS_ENABLE_MLT=ON -DCS_MLT_ROOT=D:\Projects\STUDIO\.worktrees\r1-03-mlt-preview\build\mlt\prefix -DCS_WARNINGS_AS_ERRORS=ON'
& $env:ComSpec /d /c 'C:\PROGRA~2\MICROS~2\2022\BUILDT~1\VC\Auxiliary\Build\vcvars64.bat >nul && C:\PROGRA~2\MICROS~2\2022\BUILDT~1\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe --build build\windows-mlt-audit --parallel 4'
```

Expected: zero compiler warnings and complete build success.

- [x] **Step 6: Run complete sequential CTest and shipping audit**

Run the exact sequential gate with Qt/MLT runtime PATH:

```powershell
& $env:ComSpec /d /c 'C:\PROGRA~2\MICROS~2\2022\BUILDT~1\VC\Auxiliary\Build\vcvars64.bat >nul && set "PATH=C:\Qt\6.8.3\msvc2022_64\bin;D:\Projects\STUDIO\.worktrees\r1-03-mlt-preview\build\mlt\prefix\bin;%PATH%" && C:\PROGRA~2\MICROS~2\2022\BUILDT~1\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe --test-dir build\windows-mlt-audit --output-on-failure -j1'
```

Expected: 100 percent pass, zero failures/skips. Launch `build/windows-mlt-audit/creator_studio.exe`, confirm `Responding=True`, and inspect the shipping link/runtime so `cs_fakes`, test objects, `melt`, and unaudited MLT modules are absent.

- [x] **Step 7: Self-review and correct every confirmed finding**

Use `superpowers:requesting-code-review`; because new delegation is disabled for this run, perform a fresh-diff self-review against the approved design. For every confirmed issue, add a failing regression test, fix it, and rerun focused plus full gates.

- [ ] **Step 8: Record evidence and integrate**

Write exact commands/counts/timings/platform limits/license boundary to the verification report. Mark only R1-05 complete in the roadmap. Commit documentation, then use `superpowers:finishing-a-development-branch` to merge `feat/r1-05-effects-titles-captions` into `feat/r1-usable-recorder-editor`, preserving user-owned untracked files. Re-run the complete sequential audited gate on the parent branch.

Commit: `docs(r1-05): record effects and text verification`
