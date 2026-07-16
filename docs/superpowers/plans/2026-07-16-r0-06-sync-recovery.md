# R0-06 Synchronization and Recovery Implementation Plan

> Execute in order with test-driven development. Commit after each green task.

## Goal

Keep independently clocked live tracks aligned to an audio master and make interrupted
two-second recording state recoverable without deleting or overwriting partial media.

## Task 1: Add the Qt-free synchronization module

- Add `cs_sync`, `ClockCoordinator`, source registration, master selection, bounded
  offset/rate filtering, correction snapshots, and overflow checks.
- Test microphone/system/video master priority, callback interpolation, constant offset,
  positive/negative ppm, clamping, duplicate observations, and two-hour arithmetic.

## Task 2: Add the bounded video synchronization planner

- Add a pure `VideoSyncPlanner` returning pass/drop/duplicate outputs.
- Retain only the last frame and fixed counters; cap duplicate emission per callback.
- Test exact boundaries, late drop, forward-gap duplicate, large discontinuity, shared
  platform-handle lifetime, and monotonic output.

## Task 3: Carry neutral audio rate correction

- Add default-one `sampleRateRatio` to `AudioBlock` and validation helpers.
- Extend FFmpeg audio encoding with fractional compensation accumulation.
- Test invalid ratios, corrections below one sample per block, positive/negative drift,
  mono/stereo, and decoded duration/sample evidence.

## Task 4: Integrate synchronization into production recording

- Create one coordinator per live take and register every active source before attach.
- Observe master/follower blocks at callback receipt, correct timestamps, run video
  planning, and pass audio correction ratios to track workers.
- Add engine diagnostics for drift, resampling ppm, sync drops, and duplicates.
- Marshal them through `LiveRecordingController` and QML without merging capture,
  queue, or sync drop meanings.

## Task 5: Expose interrupted segment metadata

- Add a read-only SQLite query for WRITING segment identities and paths by session.
- Validate all recovered relative paths using the same package containment rules.
- Test unicode, multiple sources, malformed DB paths, and already recovered sessions.

## Task 6: Implement idempotent `.part` quarantine

- Add package-local no-overwrite move operations with symlink/reparse/hard-link checks.
- Quarantine known WRITING parts before the DB recovery transaction.
- Quarantine unreferenced `.part` orphans without interpreting them as playable media.
- Test partial retry, destination conflict, missing source, I/O failure, and containment.

## Task 7: Strengthen the real process-death fixture

- Make the child create one durable READY file and one actual WRITING `.part`, then
  call `std::_Exit(73)` while SQLite/WAL state is live.
- Verify recovery preserves READY bytes, moves the `.part`, marks the row FAILED,
  reports no subsequent candidate, and never overwrites quarantine.
- Assert recovered committed loss is at most the configured two-second segment.

## Task 8: Run two-hour drift acceptance

- Accelerate a two-hour microphone-master plus drifting camera and secondary-audio run.
- Exercise both signs of oscillator error and callback jitter without sleep.
- Verify final absolute timestamp drift <= 40 ms, bounded ratios, monotonic video PTS,
  and stable fixed-size coordinator state.

## Task 9: Review and close R0-06

- Run warning-as-error Debug and Release builds with and without FFmpeg.
- Run low-level, application, QML, FFmpeg playback, two-hour, and process-death suites.
- Search for wall-clock synchronization, unbounded history, swallowed corrections,
  unsafe filesystem moves, and stale `.part` files.
- Update README/progress with physical macOS acceptance boundaries and complete the
  R0-02 through R0-06 integration record.
