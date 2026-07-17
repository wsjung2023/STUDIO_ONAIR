# R1-05 Effects, Titles, and Basic Captions Verification

## Delivered boundary

- Durable visual transform, PIP preset, crop, rotation, opacity, and z-order.
- Durable microphone gain and clip-local fade-in/fade-out.
- Durable Unicode title and caption cue tracks with deterministic generated PNG
  overlays, exact reopen, undo/redo, and explicit save behavior.
- Audited MLT preview applies the durable visual/audio/text state without adding
  an unaudited plugin or external process.
- Editor inspector exposes validated effect, title, and caption operations with
  keyboard and accessibility coverage.

This closes only R1-05. Studio scene/recording integration, export, and the final
30-minute capture-edit-export product gate remain later R1 delivery items.

## Physical acceptance

`cs_r1_effects_text_acceptance_tests` contains four physical tests. Each test has
a 45-second CTest deadline and a PID-qualified Unicode temporary package path.
Package teardown asserts that Windows releases every database, cache, and MLT
file handle.

The tests prove:

- a real Unicode `.cstudio` package, SQLite store, generated cache,
  `EditorController` workers, and audited MLT engine survive save, destruction,
  reopen, and exact comparison;
- camera PIP plus manual transform, microphone envelope, a Korean title, and two
  Korean caption cues produce real preview pixels and expected PCM amplitude;
- four undo and four redo operations cross audio and text edits durably;
- repeated SQLite commit failure changes no revision, model, cache, or engine;
- repeated derived-cache failure preserves the durable commit, reports stale
  preview, and regenerates successfully after full reopen;
- a 30-minute, 72-visual-branch graph loads and seeks through the real
  `EditorController` scheduling path while the UI event loop remains responsive.

Measured representative run:

```text
graph_build_ms=1727
frame_request_ms=46
visual_branches=72
composite_transitions=72
event_loop_passes=127
max_ui_gap_ms=16
working_set_delta_bytes=30474240
```

Budgets are graph build below 10 seconds, frame request below 3 seconds, maximum
UI gap below 250 ms, and Windows working-set growth below 768 MiB.

The four physical tests passed five consecutive `ctest -j4` repetitions. Their
real times per repetition were 6.59, 6.87, 7.14, 6.79, and 7.27 seconds, with
4/4 passing each time. This also exercises the parallel-process package boundary.

## Clean build and complete gate

- Fresh Windows audit directory configured with Ninja, Qt 6.8.3, MLT enabled,
  tests enabled, and `CS_WARNINGS_AS_ERRORS=ON`.
- MSVC `/W4 /permissive- /utf-8 /WX`: successful build, zero compiler warnings.
- Final incremental audit: `ninja: no work to do.`
- Final sequential CTest: **579/579 passed**, zero failed, in 113.73 seconds.
- Source scan: no TODO, FIXME, HACK, disabled/skipped test, or permissive
  `EXPECT_TRUE(true)`/`ASSERT_TRUE(true)` marker in the R1-05 change set.
- Application launch: `creator_studio`, `Responding=True`, alive at the probe
  point and then stopped by the verification harness.

Qt 6.8.3 generates an unreachable-code warning in native AOT C++ for the
intentionally dynamic `EditorPage` controller boundary. `QT_QML_SKIP_CACHEGEN`
is restricted to `qml/EditorPage.qml`; all other shipping QML pages retain their
normal native AOT generation. The audit build proves the file-scoped boundary
under `/WX`.

## Shipping and commercial OSS audit

- Direct imports are Qt debug, MSVC debug runtime, and Windows system DLLs.
- Only `mlt++-7.dll` and `mlt-7.dll` are delay-loaded.
- Forbidden application link inputs (`cs_fakes`, tests, gtest/gmock,
  `melt.exe`): **0**.
- Forbidden staged runtime files (`melt.exe`, GPL/JACK/extra Qt module matches):
  **0**.
- The pinned MLT 7.40.0 boundary remains dynamic LGPL `core` plus `avformat`.
  R1-05 intentionally does not use MLT `normalize`, `plus`, or `qt6` modules;
  transforms, envelopes, and text rasterization stay in product code and the
  existing Qt application boundary.

This is engineering provenance evidence, not a substitute for final legal,
codec patent/royalty, or store-policy review.

## Review closure and platform limits

Independent review findings were corrected and re-reviewed to no remaining
findings: deterministic test deadlines, actual controller worker responsiveness,
teardown handle verification, file-scoped QML cachegen exclusion, parallel-safe
temporary paths, and direct `seek()` UI-call timing.

- Windows x64 is the physically verified R1-05 platform.
- macOS compilation and physical MLT verification are not available on this
  machine and are not claimed.
- The acceptance records the resolved system font family. Bundling and OFL
  evidence for a deterministic shipping font remain an R4 packaging decision.
- The audit is a Debug verification package; Release deployment, signing,
  installer layout, and redistributable packaging remain R4.
