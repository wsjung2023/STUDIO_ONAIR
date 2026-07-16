# R0-05 Source-Separated Recording Implementation Plan

> Execute in order with test-driven development. Commit after each green task.

## Goal

Build source-separated, durable, bounded FFmpeg recording with two-second
segments, live SQLite segment state, disk-space protection, and playable-media
integration evidence.

## Task 1: Pin and audit the FFmpeg dependency

- Add a reproducible source-build script for FFmpeg 8.1.2.
- Verify the official archive signature/hash.
- Refuse GPL/nonfree configure flags and shared-library-off builds.
- Add CMake discovery that keeps normal builds working when FFmpeg is disabled
  and builds `cs_ffmpeg_adapter` when an audited root is supplied.
- Add a capability probe and configuration/license evidence test.

## Task 2: Define track, path, encoder, and lifecycle ports

- Add `RecordingTrack`, role/media enums, safe relative-path policy, and segment
  descriptors.
- Add `ITrackSegmentEncoder`, `IVideoFrameMapper`, and
  `ISegmentLifecycleSink` without FFmpeg/project-store types.
- Test every role path, unsafe ID rejection/encoding, monotonically increasing
  indices, and port ownership rules.

## Task 3: Implement disk-space monitoring

- Add injectable filesystem-space probe and reserve/estimate policy.
- Test sufficient, reserve-boundary, insufficient, overflow, and probe-error
  cases.
- Expose immutable diagnostics snapshots.

## Task 4: Implement durable segment publication

- Add package-contained `.part` creation, fsync/close, no-overwrite atomic
  publish, and directory sync abstraction.
- Enforce begin -> bytes -> durable rename -> ready ordering.
- Test each injected failure point and exactly-once failed notification.

## Task 5: Implement the bounded asynchronous track recorder

- Add bounded newest-video and FIFO-audio queues.
- Put all encoder, filesystem, and lifecycle work on one worker per track.
- Segment strictly from project timestamps at two-second boundaries.
- Flush the trailing segment and coalesce concurrent stop completions.
- Test video-drop counters, audio overflow failure, worker failure propagation,
  restart prohibition, and teardown.

## Task 6: Connect per-segment SQLite persistence

- Implement the application-owned `ProjectSegmentLifecycleSink` over
  `IProjectPackageStore`.
- Serialize state changes per session and aggregate READY segments into the
  final `RecordingSession`.
- Integration-test that DB rows and files agree after normal stop and injected
  failures.

## Task 7: Implement the FFmpeg video segment encoder

- Confine all FFmpeg includes and ownership wrappers to `src/ffmpeg_adapter`.
- Probe H.264 hardware encoders, use the documented LGPL fallback, convert BGRA
  when needed, force the first keyframe, and mux standalone Matroska files.
- Test open/send/receive/flush/error paths with synthetic frames.

## Task 8: Implement the FFmpeg audio segment encoder

- Accept float32 interleaved blocks, use libswresample for the chosen AAC
  format, preserve project PTS, pad only the final codec frame when required,
  and mux standalone MKA files.
- Test mono/stereo, block sizes, trailing partial codec frames, discontinuities,
  and error propagation.

## Task 9: Add platform frame mapping and multi-track orchestration

- Implement a CPU synthetic mapper for integration tests and a macOS retained
  CVPixelBuffer BGRA mapper.
- Add `MultiTrackRecordingService` for screen, camera, microphone, system audio,
  and optional composite preview.
- Ensure one track failure stops the take visibly without corrupting other
  finalized segments.

## Task 10: Wire application state and QML diagnostics

- Replace production fake recording only when a project and audited FFmpeg
  runtime are available; preserve an explicit unsupported status otherwise.
- Marshal async start/stop, per-track queue/drop/segment stats, encoder choice,
  disk bytes, and errors to the UI thread.
- Add composite-recording opt-in and QML contract tests.

## Task 11: Run playback, index, and long-timeline acceptance

- Generate real synthetic FFmpeg video/audio segments and decode each final
  file through FFmpeg.
- Compare every READY DB row to the media tree and reject extra/missing files.
- Run a timestamp-accelerated 30-minute multi-track scenario and record size,
  segment count, duration, and decode evidence.
- If hardware is available, run a real-time soak; otherwise state that boundary.

## Task 12: Review and close R0-05

- Run Debug and Release warning-as-error builds and complete test suites.
- Search for unbounded queues, sleeps, swallowed errors, raw FFmpeg types above
  the adapter, and GPL/nonfree flags.
- Request independent Critical/Important review, fix findings, re-run evidence,
  update README/progress, and proceed directly to R0-06.
