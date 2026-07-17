# R1-06 Export and R1-07 Closure Implementation Plan

> Execute this plan test-first. R2 does not start or merge until every R1-06
> automated/physical gate and the R1-07 long-form product gate have evidence.

**Goal:** Deliver revision-frozen H.264/AAC MP4 export with durable recovery,
hardware probing and software fallback, then close R1 with a real 30-minute
capture-edit-export A/V synchronization run.

**Architecture:** Extend the Qt-free render values and port, persist render jobs
in SQLite migration 004, render an independent frozen MLT graph through the
audited in-process `avformat` consumer, and expose it through an asynchronous Qt
controller and accessible Export page. The final R1-07 gate consumes the same
shipping application and output probe; it does not use a test-only render path.

**Commercial boundary:** Reuse audited dynamic LGPL MLT/FFmpeg. Do not add
`libx264`, GPL/nonfree flags, subprocess FFmpeg, cloud dependencies, or a web
runtime. Windows is the physical release target in this workspace; macOS claims
remain gated on native hardware.

---

## Task 1: Validated render contract and immutable presets

**Files:**
- Modify: `src/edit_engine/EditEngineTypes.h`
- Modify: `src/edit_engine/EditEngineTypes.cpp`
- Modify: `tests/edit_engine/EditEngineTypesTest.cpp`

1. Add red tests for canonical preset IDs `h264-1080p30` and
   `h264-2160p30`, project identity, frozen revision, absolute `.mp4`
   destination validation, overwrite policy, terminal states, and monotonic
   progress.
2. Run `cs_tests.exe --gtest_filter=EditEngineTypesTest.*`; confirm the new API
   is absent or tests fail for the intended validation.
3. Implement the smallest Qt-free values. Preserve the existing public API only
   where compatibility is useful; reject relative/device/traversal paths and
   non-existing or reparse-point parents.
4. Rerun the focused tests and commit `feat(export): define immutable render contract`.

## Task 2: Migration 004 and durable render-job store

**Files:**
- Create: `src/project_store/migrations/004_render_jobs.sql`
- Create: `cmake/Migration004.h.in`
- Modify: `CMakeLists.txt`
- Modify: `src/project_store/MigrationRunner.cpp`
- Create: `src/project_store/IRenderJobStore.h`
- Create: `src/project_store/SqliteRenderJobStore.h`
- Create: `src/project_store/SqliteRenderJobStore.cpp`
- Modify: `src/project_store/CMakeLists.txt`
- Modify: `tests/project_store/MigrationRunnerTest.cpp`
- Create: `tests/project_store/SqliteRenderJobStoreTest.cpp`
- Modify: `tests/CMakeLists.txt`

1. Add red tests for schema version/checksum, exact idempotent begin, allowed
   state transitions, immutable terminals, monotonic progress, publishing hash,
   stale-running recovery, and stale-publishing reconciliation inputs.
2. Run the migration/store filters; confirm schema remains version 3 and the
   store is missing.
3. Add checksum-pinned migration 004 and a Qt-free store with transactions and
   SQL constraints/triggers. `pending -> running -> publishing -> completed`,
   cancellation, and failure are the only legal paths.
4. Reopen real temporary databases and prove committed rows survive; run all
   project-store tests and commit `feat(export-store): persist recoverable render jobs`.

## Task 3: Encoder capability preflight and deterministic selection

**Files:**
- Create: `src/mlt_adapter/ExportEncoderProbe.h`
- Create: `src/mlt_adapter/ExportEncoderProbe.cpp`
- Modify: `src/mlt_adapter/CMakeLists.txt`
- Create: `tests/mlt_adapter/ExportEncoderProbeTest.cpp`
- Modify: `tests/CMakeLists.txt`

1. Add red seam tests for ordered NVENC/QSV/Media Foundation hardware
   candidates, forced `h264_mf` software fallback, evidence per preset, maximum
   tested raster, and rejection of name-only capabilities.
2. Add a physical short black-frame/silent-audio preflight that writes only to
   a private temporary directory and validates the result in-process.
3. Implement injected candidate discovery/selection; never select a candidate
   that did not complete a real preset preflight.
4. Run seam and physical tests for 1080p and supported 4K. Record unavailable
   hardware as evidence, not failure, when the software fallback passes. Commit
   `feat(export): preflight h264 encoder capabilities`.

## Task 4: Independent MLT render job, cancellation, and atomic publication

**Files:**
- Create: `src/mlt_adapter/MltRenderJob.h`
- Create: `src/mlt_adapter/MltRenderJob.cpp`
- Modify: `src/mlt_adapter/MltEditEngine.cpp`
- Modify: `src/mlt_adapter/CMakeLists.txt`
- Create: `tests/mlt_adapter/MltRenderJobTest.cpp`
- Modify: `tests/mlt_adapter/MltEditEngineTest.cpp`
- Modify: `tests/CMakeLists.txt`

1. Add red tests proving the job owns an immutable snapshot, uses the shared
   graph plan, never borrows preview state, and joins its worker on destruction.
2. Add cancellation/failure injection tests at preflight, graph build, encode,
   flush, probe, publishing, and final database commit. Assert the destination
   is absent and owned partials are removed/quarantined.
3. Implement the MLT `avformat` consumer with exact profile, H.264/AAC settings,
   bounded progress publication, early hardware-to-software retry, no frame
   dropping, and idempotent cancellation.
4. Probe H.264, AAC, raster, rate, packets, pixel content, PCM, and duration
   before persisting publishing intent. Flush/close, hash, and atomically rename
   on the same volume; completed is only written after publication.
5. Run repeated destruction under the CRT debug gate and commit
   `feat(export): render frozen timelines atomically`.

## Task 5: Recovery coordinator and destination safety

**Files:**
- Create: `src/project_store/RenderJobRecovery.h`
- Create: `src/project_store/RenderJobRecovery.cpp`
- Modify: `src/project_store/CMakeLists.txt`
- Create: `tests/project_store/RenderJobRecoveryTest.cpp`
- Modify: `tests/CMakeLists.txt`

1. Add red crash-reopen tests for stale pending/running/cancelling, publishing
   with matching final, publishing with matching partial, mismatched identities,
   hard links/reparse points, and unrelated files.
2. Implement exact path, file identity, size, and SHA-256 reconciliation.
   Recovery may touch only the artifact named by the durable row.
3. Run the helper-process crash fixtures and commit
   `feat(export-store): reconcile interrupted publication safely`.

## Task 6: Asynchronous application controller

**Files:**
- Create: `src/app/ExportWorker.h`
- Create: `src/app/ExportWorker.cpp`
- Create: `src/app/ExportController.h`
- Create: `src/app/ExportController.cpp`
- Modify: `src/app/CMakeLists.txt`
- Create: `tests/app/ExportControllerTest.cpp`
- Modify: `tests/CMakeLists.txt`

1. Add red tests for presets, destination, start/cancel/retry, monotonic
   progress, selected encoder/fallback diagnostics, terminal action parity,
   page-close continuation, app-close drain, and repeated destruction.
2. Implement a dedicated worker thread. All engine/store calls stay off the GUI
   thread and every terminal path joins; no `QThread` survives destruction.
3. Integrate project snapshot freezing without blocking edits after acceptance
   of the job. Run focused tests including 100 destruction loops and commit
   `feat(export-ui): add asynchronous export controller`.

## Task 7: Accessible Export page and shipping composition

**Files:**
- Create: `qml/ExportPage.qml`
- Modify: `qml/Main.qml`
- Modify: `CMakeLists.txt`
- Modify: `src/main.cpp`
- Modify: `tests/app/QmlSmokeTest.cpp`
- Create: `tests/acceptance/R1ExportUiAcceptanceTest.cpp`
- Modify: `tests/CMakeLists.txt`

1. Add red QML tests for navigation, keyboard access, invalid/busy disabling,
   cancel visibility, status/diagnostic text, progress below 100% before atomic
   publication, retry, and reveal action.
2. Add the model-driven Export page and construct the real controller/store/MLT
   dependencies in the shipping executable.
3. Run offscreen QML tests and a hidden app responsiveness probe; commit
   `feat(export-ui): ship accessible export workflow`.

## Task 8: R1-06 media acceptance and audit

**Files:**
- Create: `tests/acceptance/R1ExportAcceptanceTest.cpp`
- Create: `docs/verification/r1-06-export-verification.md`
- Modify: `tests/CMakeLists.txt`
- Modify: `IMPLEMENTATION_ROADMAP.md`
- Modify: `legal/OSS_BOM.csv` only if the audited dependency graph changed

1. Create a Unicode-path project with screen, camera PIP, two audio sources,
   cuts, transform, title, caption, gain, and fades using the same persisted
   project path as the app.
2. Export 1080p and the supported 4K preset through the shipping composition.
   Verify streams, raster/rate/duration, representative pixels, PCM, A/V offset,
   fallback diagnostics, no partials, and app responsiveness.
3. Run `/W4 /permissive- /utf-8 /WX`, sequential and parallel complete CTest,
   import/link audits, forbidden-runtime scan, and hidden app probe.
4. Review every changed file and add a regression test for each confirmed
   issue. Mark R1-06 complete only after all evidence is recorded; commit
   `test(r1-06): verify production export workflow`.

## Task 9: R1-07 30-minute full product closure

**Files:**
- Create: `docs/verification/r1-07-full-product-verification.md`
- Modify: `IMPLEMENTATION_ROADMAP.md`
- Add or modify regression tests only for defects actually found

1. Preflight real Windows screen, camera, microphone, system audio, free disk,
   encoder, MLT runtime, and output destination. A missing physical capability
   is a blocker to fix or explicitly obtain; it is never silently faked.
2. Record a continuous 30-minute screen lesson through the shipping Studio with
   camera and two audio sources. Record pause/resume, scene transition, marker,
   source separation, timestamps, and recovery evidence.
3. Reopen the package, remove several mistake ranges, ripple delete, move/crop
   camera PIP, adjust gain/fades, and add Unicode title/captions. Save, close,
   reopen, and prove the exact timeline revision survives.
4. Export H.264/AAC MP4 through the shipping Export page. Probe full duration,
   packet monotonicity, A/V start/end drift, edits, overlays, and representative
   frames/audio at the beginning, middle, and end.
5. Run cancellation/retry once, repeat the complete automated suite, verify no
   security dialog, debug assertion, leaked thread, orphan partial, or forbidden
   DLL, and record exact commands/counts/timings.
6. Fix every confirmed defect test-first and repeat the affected physical gate.
   Mark R1-07 complete only when no required item remains. Commit
   `test(r1-07): close full capture edit export workflow`.

## Task 10: Integrate R1 and then resume R2

1. Use the finishing-development-branch checklist, verify the R1 branch against
   its base, and fast-forward/merge only reviewed commits.
2. Rerun the complete integrated gate from the destination branch.
3. Rebase or recreate `feat/r2-01-cursor-telemetry` from completed R1 so its
   migration is 005, execute R2-01 through R2-07, and merge only after each R2
   gate passes.
