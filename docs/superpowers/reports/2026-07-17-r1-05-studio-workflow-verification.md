# R1-05 Studio Workflow Verification

## Delivered boundary

- Persistent Studio scenes, source visibility/order/transform state, active
  scene switching, recording markers, configurable shortcuts, and truthful HUD.
- Source-isolated screen, camera, microphone, and system-audio recording with
  atomic, idempotent reconciliation into one durable Editor timeline revision.
- Exact scene/source/model, asset, track, clip, transform, marker, undo/redo,
  frame, and PCM behavior across destruction and reopen.
- Audited in-process FFmpeg probing and MLT preview only; no external
  `ffprobe`, `melt`, web runtime, cloud service, or new third-party dependency.

This closes R1-05 only. R1-06 export and R1-07 final 30-minute
capture-edit-export acceptance remain open, so R1 as a whole is not complete.

## Physical acceptance

`cs_r1_studio_workflow_acceptance_tests` contains four deadline-bounded
Windows x64 physical tests:

1. An in-process remuxed Matroska whose stream order is audio 0 / video 1
   proves MLT selects the first relative video stream and returns the expected
   real red frame.
2. A Unicode `.cstudio` records real MPEG-4/AAC media for screen, camera,
   microphone, and system audio; persists controller-driven scene switches and
   five markers; reconciles through SQLite; reopens exact Studio and Editor
   state; returns real MLT pixels/PCM; and round-trips composite undo/redo.
3. Repeated missing and corrupt physical media failures leave the SQLite file
   byte-identical and publish no partial assets/tracks. Restoring the valid file
   retries successfully. Three controller destructions during queued reopen
   complete without a running-thread failure.
4. A real 30-minute scale package uses 120 physical 15-second media files, 60
   scene switches, and 300 markers through the real Studio worker, FFmpeg probe,
   reconciliation transaction, Editor worker, and two MLT graph opens.

All four tests passed five consecutive in-process repetitions: **20/20** in
222.1 seconds, with no skip, disabled test, security block, debug exception, or
`QThread: Destroyed while thread is still running` failure.

Representative fifth repetition:

```text
import_ms=11044
graph_build_ms=2678
frame_request_ms=4
max_ui_gap_ms=62
handles_before=7281
handles_after_first=7400
handles_after_reopen=7519
working_set_delta_bytes=4358144
```

Budgets are import below 30 seconds, graph build below 10 seconds, frame request
below 3 seconds, maximum UI gap below 250 ms, working-set growth below 768 MiB,
process handle ceiling 16,384, first 120-clip graph growth at most 6,144, and
same-snapshot graph reopen growth at most 192. The MLT/pthreads Windows runtime
retains a process-lifetime event pool; the absolute and repeated-open ceilings
distinguish that bounded warm-up from runaway graph growth.

## Defects found by the physical gate

The acceptance work found and fixed product defects rather than weakening the
tests:

- Concurrent screen and camera H.264 hardware sessions could advertise support
  and then reject the second encoder. Multi-video capture now uses the audited
  LGPL software MPEG-4 encoder, and availability requires MPEG-4 plus AAC.
- Corrected audio timestamps could move backward or overlap after independent
  source callbacks. The clock is monotonic per source and audio blocks clamp to
  the previous physical block end.
- Requested segment duration could exceed the finalized container duration.
  Encoders now reopen the finalized file and publish the bounded physical
  duration, including the one-frame video boundary.
- Absolute MLT stream index 0 blanked valid containers ordered audio 0 / video
  1. Audio and visual producers now use relative `astream`/`vstream` selectors;
  the physical mixed-stream regression was red before and green after.
- Parallel generated-overlay tests shared PID-less temporary paths. PID-qualified
  roots passed 40/40 focused parallel repetitions.
- The integrated parallel gate exposed the same process-collision class in the
  durable Editor acceptance and shortcut-settings tests. Their roots now include
  the process ID; the three previously failing tests passed 15/15 focused
  parallel repetitions before the complete gate passed.

## Build and complete gates

- MSVC Debug build under `/W4 /permissive- /utf-8 /WX`: successful, zero
  compiler warnings.
- Complete sequential CTest: **717/717 passed**, zero failed, in 320.88 seconds.
- Complete parallel CTest (`-j4`): **717/717 passed**, zero failed, in 114.09
  seconds.
- Integrated parent-branch audited CTest (`-j4`): **717/717 passed**, zero
  failed, in 139.00 seconds with FFmpeg and MLT enabled.
- Hidden application launch: `creator_studio`, `Responding=True`, alive at the
  three-second probe point, then stopped by the verification harness.
- `git diff --check`: clean.

## Shipping and commercial OSS audit

- Application imports are audited FFmpeg (`avcodec`, `avformat`, `avutil`,
  `swresample`, `swscale`), dynamic MLT (`mlt++-7`, `mlt-7`), Qt, MSVC debug
  runtime, and Windows system libraries.
- Forbidden staged files (`melt.exe`, GPL/JACK/Qt MLT modules): **0**.
- Product external media-process launch references: **0**. The only `melt`
  source reference is the runtime manifest's explicit rejection rule.
- Product link command includes `cs_fakes`, gtest/gmock, or test objects: **0**.
- Multi-source software MPEG-4 and AAC stay inside the existing audited dynamic
  LGPL FFmpeg boundary. This is engineering provenance evidence, not a
  substitute for final codec patent/royalty, store-policy, or legal review.

## Independent review and platform limits

Independent review first found the absolute MLT stream-index defect. The
physical audio-first/video-second regression and relative selector fix were
reviewed again with result **CLEAN**; no other requested acceptance gap remained.

- Windows x64 is the physically verified R1-05 platform.
- macOS compilation and physical capture/MLT verification are unavailable on
  this machine and are not claimed.
- This is a Debug verification build. Release packaging, signing, installer,
  redistributables, and store submission remain R4 work.
