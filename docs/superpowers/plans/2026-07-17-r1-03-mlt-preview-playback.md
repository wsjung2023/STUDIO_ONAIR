# R1-03 Audited MLT Preview and Playback Implementation Plan

> **Execution:** Follow `superpowers:executing-plans`, `superpowers:test-driven-development`, and `superpowers:verification-before-completion`. Keep every public-header change followed by a clean MSVC build because localized include tracking is not reliable.

**Goal:** Replace the shipping editor's unavailable engine with an audited, dynamically linked MLT adapter that compiles Creator Studio timelines into real MLT graphs, decodes real preview frames, drives playback off the editor worker, and displays those frames in QML without exposing MLT types outside the adapter.

**Architecture:** MLT remains a disposable render cache behind `IEditEngine`; the domain snapshot remains authoritative. A product-owned runtime manifest pins MLT v7.40.0 commit `bef9d89c0c279e558d9625dac3399c2aa3d961bc`, permits only the LGPL framework, mlt++, `core`, and `avformat` binaries/data, and verifies hashes before `mlt_factory_init` is called with an explicit repository path. The adapter compiles immutable snapshots into an MLT-neutral graph plan first, then owns and destroys the native graph deterministically on the existing editor worker thread. Playback is implemented as bounded asynchronous frame requests; the UI thread only receives copied `QImage` frames.

**Commercial boundary:** Build dynamic libraries from the pinned source with `GPL=OFF`, `GPL3=OFF`, `USE_AVDEVICE=OFF`, and every optional module OFF except `MOD_AVFORMAT`. Reuse the audited dynamic LGPL FFmpeg 8.1.2 prefix. Do not build, copy, install, invoke, or distribute `melt`. Treat codec patents/royalties as a separate R4 release gate.

**Primary stack:** C++20, Qt 6.8 Quick, MLT 7.40 C API/MLT++, FFmpeg 8.1.2 dynamic libraries, CMake 3.25+, GoogleTest.

---

## Task 1: Pin and audit the MLT dependency

**Files:**
- Create: `scripts/bootstrap_mlt.ps1`
- Create: `scripts/verify_mlt_runtime.ps1`
- Create: `cmake/FindMLT.cmake`
- Modify: `legal/OSS_BOM.csv`
- Modify: `docs/adr/0003-ffmpeg-mlt-boundaries.md`
- Test: `tests/scripts/MltBootstrapPolicyTest.ps1`

1. Write a failing policy test that requires the exact version, commit, archive hash, dynamic build flags, all forbidden module flags, selected-copy packaging, and evidence/manifest generation.
2. Implement a fail-closed bootstrap that downloads and hashes the official source archive, checks the extracted commit-equivalent source, installs missing build-only vcpkg dependencies at the existing pinned vcpkg commit, and builds only selected MLT targets.
3. Copy only allowlisted DLLs/import libraries/headers/module data into `build/mlt/prefix`; generate a UTF-8 JSON manifest with relative path, SHA-256, role, upstream version, commit, and license classification.
4. Implement a standalone runtime verifier that rejects missing, changed, duplicate, unexpected, GPL-named, or executable artifacts.
5. Run the policy test, bootstrap, verifier, and a negative tamper test.

## Task 2: Define deterministic graph compilation

**Files:**
- Modify: `src/edit_engine/EditEngineTypes.h`
- Create: `src/mlt_adapter/MltGraphPlan.h`
- Create: `src/mlt_adapter/MltGraphPlan.cpp`
- Create: `src/mlt_adapter/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Test: `tests/mlt_adapter/MltGraphPlanTest.cpp`
- Modify: `tests/CMakeLists.txt`

1. Write failing tests for empty timelines, gaps, overlapping video layers, audio tracks, disabled/offline clips, trim ranges, Unicode paths, and stable track order.
2. Extend `TimelineSnapshot` with the immutable asset catalog and package media root while preserving default construction for existing tests.
3. Add Qt-free graph-plan values containing resolved canonical media paths, source/timeline frame ranges, track kind/order, enable state, transform/audio metadata, and revision.
4. Compile nanosecond domain time to MLT frame positions with checked rational arithmetic and reject escapes outside the package root, missing assets, duplicate asset IDs, unsupported title/caption clips, and overflow.
5. Keep the graph compiler independently testable with MLT disabled.

## Task 3: Enforce the runtime boundary

**Files:**
- Create: `src/mlt_adapter/MltRuntimeManifest.h`
- Create: `src/mlt_adapter/MltRuntimeManifest.cpp`
- Test: `tests/mlt_adapter/MltRuntimeManifestTest.cpp`

1. Write failing tests for the valid manifest plus missing, extra, wrong-hash, wrong-version, path-traversal, symlink/reparse-point, executable, and forbidden-module cases.
2. Parse the generated manifest without Qt, canonicalize all paths, enumerate the runtime root, and compare the complete file set before loading any library/module.
3. Require exactly the adapter ABI version and MLT source identity compiled into Creator Studio.
4. Return contextual `AppError` values without leaking absolute user paths into normal UI messages.

## Task 4: Implement `MltEditEngine`

**Files:**
- Create: `src/mlt_adapter/MltEditEngine.h`
- Create: `src/mlt_adapter/MltEditEngine.cpp`
- Create: `src/mlt_adapter/MltHandles.h`
- Test: `tests/mlt_adapter/MltEditEngineTest.cpp`

1. Write failing physical tests using a generated lossless image/video fixture and the audited runtime for load, seek, BGRA frame decode, revision propagation, play/pause state, update rebuild, malformed media, and repeated destruction.
2. Verify the manifest, initialize `mlt_factory_init` with the explicit product module directory, and verify repository services needed by the plan (`loader`, `avformat`, `colour`, `blank`, `tractor`, `playlist`).
3. Build playlists and a tractor from the graph plan. Insert explicit blanks for gaps, trim producer cuts, connect tracks in stable order, and add only reviewed core transitions needed for video compositing/audio mixing.
4. Implement frame seeking and `mlt_frame_get_image(..., mlt_image_bgra, ...)`, copy the bounded pixel buffer into an owning neutral `VideoFrame`, and close every frame/producer/playlist/tractor/profile/repository handle on all paths.
5. Implement `update` as deterministic full rebuild for unsupported incremental changes while preserving the last good graph if construction fails. Leave render jobs for R1-05 but return an explicit unsupported error, never a fake success.

## Task 5: Wire asynchronous preview and playback into the app

**Files:**
- Modify: `src/app/EditorEngineWorker.h`
- Modify: `src/app/EditorEngineWorker.cpp`
- Modify: `src/app/EditorController.h`
- Modify: `src/app/EditorController.cpp`
- Create: `src/app/EditorPreviewItem.h`
- Create: `src/app/EditorPreviewItem.cpp`
- Modify: `src/app/CMakeLists.txt`
- Modify: `src/main.cpp`
- Modify: `qml/EditorPage.qml`
- Test: `tests/app/EditorControllerTest.cpp`
- Test: `tests/app/QmlSmokeTest.cpp`

1. Write failing tests that prove frame requests execute off the UI thread, playback advances at the timeline rate, only one request is in flight, late frames are dropped by generation/revision, pause stops advancement, seek requests the exact frame, and engine failures make preview stale without losing the durable edit.
2. Add a frame operation to the serialized worker queue. Convert a neutral owned BGRA frame to a detached `QImage` on the worker and publish it with generation/revision/position metadata.
3. Drive playback with a monotonic Qt timer and bounded backpressure; never queue an unbounded frame backlog.
4. Add a portable `QQuickPaintedItem` preview surface that preserves aspect ratio and paints a deterministic unavailable/stale state.
5. Register the preview type, construct `MltEditEngine` only when `CS_ENABLE_MLT=ON`, retain `UnavailableEditEngine` for dependency-free developer builds, and make the QML transport/play/seek controls reflect the real controller state.

## Task 6: Physical acceptance and release evidence

**Files:**
- Create: `tests/acceptance/R1MltPreviewAcceptanceTest.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `docs/IMPLEMENTATION_ROADMAP.md`
- Create: `docs/superpowers/reports/2026-07-17-r1-03-verification.md`

1. Generate a two-colour/two-track media fixture, open a Unicode package path, compile the real graph, request frames before and after a cut, play/seek/pause through the controller, and verify exact pixel regions and bounded timing.
2. Tamper with one module and add one forbidden extra DLL; prove startup fails closed, then restore and reverify.
3. Run a clean `/W4 /permissive- /WX` MLT-enabled build and the complete CTest suite. Inspect the shipping link graph and output directory to prove no `cs_fakes`, `melt`, GPL module, static MLT, or uncontrolled plugin path is present.
4. Launch `creator_studio.exe`, verify `responding=True`, exercise the Editor route, and record the physical result.
5. Request independent code review, address findings with regression tests, rerun the entire clean gate, update the roadmap, and commit the evidence.
