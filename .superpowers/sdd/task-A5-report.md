# Task A5 Report — FakeTrackingProvider + end-to-end pipeline

## What was built

- `src/fakes/FakeTrackingProvider.h` / `.cpp` — `creator::fakes::FakeTrackingProvider final : public avatar::ITrackingProvider`.
- `src/fakes/CMakeLists.txt` — added `FakeTrackingProvider.h/.cpp` to SOURCES, added `cs_avatar` to DEPENDS.
- `tests/avatar/FakeTrackingProviderTest.cpp` — 8 cases.
- `tests/avatar/PipelineTest.cpp` — 2 cases (the acceptance proof + a determinism/reproducibility case).
- `tests/CMakeLists.txt` — registered both new test files.

No other files were touched. `src/avatar/*` (from earlier tasks) was read but not modified.

## The fake's scripting model

`FakeTrackingProvider::ScriptedFrame` is a plain aggregate:

```cpp
struct ScriptedFrame final {
    avatar::ExpressionParameters parameters{};
    float confidence{1.0F};
    bool faceFound{true};
};
```

The constructor takes `std::vector<ScriptedFrame> script` plus an optional `AvatarProviderId`
(defaults to `"fake-tracker"`). `process(const media::VideoFrame& frame)` reads **only**
`frame.timestamp` from the argument; every other output field comes from the next unconsumed
`ScriptedFrame`. Deliberately excludes a timestamp from `ScriptedFrame` itself — process() always
takes that from the frame, never the script, which is what lets `PipelineTest` assert the frame's
timestamp flows unchanged all the way to the NDJSON line.

End-of-script behaviour: **clamp, not cycle**. Once the script is exhausted, `process()` keeps
returning the last entry on every subsequent call rather than wrapping back to index 0. Rationale
(documented in the header): a caller that overruns the script gets an obviously-repeating result
instead of silently reappearing at an earlier, unrelated frame's data. Covered by
`ClampsToFinalEntryAfterScriptIsExhausted`.

An empty script is rejected with `AppError{ErrorCode::InvalidState}` rather than fabricating a
plausible default `TrackingResult` — a configuration mistake shouldn't look like a valid reading.
Covered by `EmptyScriptFailsRatherThanReturningAPlausibleDefault`.

## FakeTrackingProviderTest (8 cases)

- `ProviderIdDefaultsToFakeTracker` / `ProviderIdCanBeOverridden`
- `EmitsScriptedParametersInOrder` — exact per-call values, including a scripted `faceFound=false`
  entry in the middle of the sequence.
- `ClampsToFinalEntryAfterScriptIsExhausted`
- `EmptyScriptFailsRatherThanReturningAPlausibleDefault` — the error path.
- `SameScriptAndFramesProduceIdenticalOutputsAcrossTwoRuns` — determinism: two independent runs of
  the same provider construction + frame sequence produce field-for-field identical
  `TrackingResult`s.
- `TimestampComesFromTheFrameNotTheScript`
- `IgnoresPixelsFromARealPixelLessCaptureFrame` — feeds a real `FakeCaptureSource::tick()` frame
  (asserted `platformHandle == nullptr` first) into `process()` and checks the scripted result
  still comes back untouched. This is a real exercise of the "ignores pixels" contract, not just a
  restated assumption: if `FakeTrackingProvider` ever dereferenced `platformHandle`, this is the
  test that would crash.

## PipelineTest (2 cases) — the acceptance proof

Wires the full chain: `FakeCaptureSource.tick()` → `FakeTrackingProvider.process()` →
`ExpressionNormalizer(CalibrationProfile)` → `AvatarMotionSample` → `AvatarMotionNdjsonSink.append()`
→ read the NDJSON back.

`FullChainProducesExactSchemaValidTelemetry`:
- 4 scripted frames, calibrated against a non-identity baseline (`eyeOpenLeft = 0.1`, the
  performer's captured resting eye) — chosen specifically so the expected output only matches if
  calibration was actually *applied*, not merely passed through.
- Frame 2 scripts `faceFound=false` with a large nonzero raw `mouthOpen=0.9`; the assertion checks
  every field of that line's `parameters` object is neutral (~0), proving the `!faceFound` path
  really flows end-to-end into telemetry rather than leaking stale/raw data.
- Asserts exactly 4 NDJSON lines, each parsed and validated against
  `schemas/event.schema.json` via the in-tree `nlohmann_json_schema_validator` (same
  collecting-error-handler pattern as `AvatarMotionSerializerTest`/`AvatarMotionNdjsonSinkTest`).
- Asserts exact (float-toleranced, 1e-4) parameter values per frame, exact `tNs` per frame against
  the frame's own timestamp, exact `provider` string, and that timestamps strictly increase frame
  to frame (proving `FakeCaptureSource`'s timing contract actually reached the sink instead of
  being dropped somewhere in the middle).

`RunningTheSameScriptTwiceProducesIdenticalNdjson`:
- Runs the identical pipeline twice into two separate temp directories and asserts the two
  resulting NDJSON files are byte-identical line-by-line — the determinism guarantee proven at the
  whole-pipeline level, not just at the fake in isolation.

Both tests use a per-test-name temp directory fixture (`SetUp`/`TearDown`) that removes itself in
`TearDown`, which gtest still runs on an assertion failure (not a crash), matching the pattern in
`AvatarMotionNdjsonSinkTest`.

## Determinism grep result

```
grep -nE '\bnow\(|sleep|std::thread|steady_clock|system_clock' src/avatar/**   -> no matches
grep -nE '\bnow\(|sleep|std::thread|steady_clock|system_clock' src/fakes/FakeTrackingProvider.h
   -> one hit, in a doc comment ("never sleeps, spawns no thread") — no actual call
grep -nE '\bnow\(|sleep|std::thread|steady_clock|system_clock' src/fakes/FakeTrackingProvider.cpp
   -> no matches
```

Confirms the whole avatar core plus the new fake read no clock, sleep, or spawn a thread anywhere.

## TDD RED/GREEN

Wrote `FakeTrackingProviderTest.cpp` and `PipelineTest.cpp` against the planned
`FakeTrackingProvider` API before writing `FakeTrackingProvider.{h,cpp}` or touching
`src/fakes/CMakeLists.txt`'s DEPENDS — this made the reconfigure/build fail first (missing
target member/header), then implementing the class and wiring `cs_avatar` into `cs_fakes`'
DEPENDS turned it green. Final `cmake --build --target cs_tests` compiled clean under
`/W4 /permissive- /WX` with no warnings; `ctest -R "FakeTrackingProviderTest|PipelineTest"` shows
10/10 passing.

## Full suite result

`ctest --preset windows-debug`: 329/331 "tests" pass; the 2 failures are the expected
`cs_app_tests_NOT_BUILT` / `cs_qml_tests_NOT_BUILT` guard entries (not built because only
`cs_tests` was built, per the task's build instructions). Baseline was 319 cs_tests cases; this
task added exactly 10 (8 + 2), landing at 329 — no regressions.

## Files changed

- `src/fakes/FakeTrackingProvider.h` (new)
- `src/fakes/FakeTrackingProvider.cpp` (new)
- `src/fakes/CMakeLists.txt` (added source + `cs_avatar` DEPENDS)
- `tests/avatar/FakeTrackingProviderTest.cpp` (new)
- `tests/avatar/PipelineTest.cpp` (new)
- `tests/CMakeLists.txt` (registered the two new test files)

## Self-review

- `cs_fakes` and `cs_avatar` stay Qt-free — `cs_add_qtfree_library` unchanged, no Qt include
  anywhere in the new files.
- `FakeTrackingProvider` follows `ITrackingProvider`'s contract exactly (deleted copy/move is
  inherited from the base; no attempt to redeclare it).
- No magic strings: provider id goes through `AvatarProviderId::create`, same as every other
  producer in this codebase.
- `[[nodiscard]] Result<...>` is respected everywhere in both new test files (no discarded
  `Result`, checked via the successful `/WX` build — an unused-result mistake here would normally
  surface as a discarded-nodiscard warning, and there were none).
- Considered whether `ScriptedFrame` needed a timestamp field per the brief's suggestion of
  "constructor-inject a vector<ExpressionParameters> plus scriptable confidence/faceFound" — chose
  one combined struct instead of three parallel vectors, since parallel vectors risk getting out of
  sync at a call site and a single struct is easier to read at each `TEST` body. Documented the
  choice in the header.
- The pipeline test's calibration baseline (`eyeOpenLeft = 0.1`) was chosen deliberately non-zero
  so the test cannot pass by accident if `ExpressionNormalizer`/`CalibrationProfile` were swapped
  for a no-op — every asserted value differs from what an unnormalized/uncalibrated run would
  produce for at least one field per frame.

## Concerns

- None blocking. One judgment call worth flagging: the "clamp at end of script" behaviour (vs.
  cycling) is my choice per the task's "your call, document it" — documented in the header and
  exercised by a dedicated test, but a future task wiring a real long-running session might prefer
  cycling; easy to change if so, since it's isolated to one `std::min` in `process()`.
- `FakeTrackingProvider::process()` never returns an error once the script is non-empty (matching
  the brief's framing of the fake as purely deterministic and never engine-failing); this mirrors
  `FakeCaptureSource::tick()`'s post-start behaviour, which also never fails once started.
