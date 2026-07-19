# R0-06 Synchronization and Recovery Verification

Date: 2026-07-17
Branch: `feat/r0-06-sync-recovery`

## Result

R0-06 code acceptance is complete on the Windows reference machine. The live
recording engine now aligns all active tracks to a deterministic audio master,
applies bounded video and audio correction, exposes separate synchronization
diagnostics, and quarantines interrupted `.part` files before committing database
recovery.

The remaining product acceptance boundary is physical Apple hardware. Windows
verification cannot compile or exercise the ScreenCaptureKit, AVFoundation,
VideoToolbox, CVPixelBuffer, or Metal paths.

## Implemented evidence

- `ClockCoordinator` uses only `ProjectClock`, selects microphone then system audio
  then video, filters drift, and clamps audio correction to +/-1000 ppm.
- `VideoSyncPlanner` emits monotonic grid timestamps, caps duplicates at two per
  callback, and keeps native capture drops distinct from synchronization drops.
- Audio correction survives the asynchronous track boundary and is applied with
  libswresample fractional compensation.
- Production QML displays maximum drift, audio correction ppm, synchronization
  drops, and duplicates separately from capture and queue counters.
- Recovery validates every WRITING path, moves known and orphan `.part` files into
  package-local no-overwrite quarantine, rejects symlinks/reparse points/hard links,
  and changes SQLite state only after filesystem handling succeeds.
- The recovery UI reports READY, interrupted-quarantine, and orphan-quarantine
  counts.

## Acceptance tests

- Accelerated two-hour microphone-master run with +500 ppm secondary audio,
  -400 ppm camera, alternating callback jitter, bounded resampling, monotonic video
  output, and final absolute corrected drift below 40 ms.
- Real child-process death via `std::_Exit(73)` with one durable READY file and one
  WRITING `.part`; recovery preserves READY bytes, quarantines the part, marks the
  row FAILED, and leaves no subsequent recovery candidate.
- Destination collision, partial retry, missing source, another active session,
  unknown session, malformed persisted path, Unicode path, hard-link alias, and
  Windows NTFS junction/reparse attacks.
- Actual FFmpeg encoded sample-count change under soft audio compensation and
  playback/decode checks for video, audio, multitrack, and long timelines.

## Reproduced verification

- Windows Debug, FFmpeg disabled, warnings as errors: build success; 358/358 tests,
  zero skips.
- Windows Release, FFmpeg disabled, warnings as errors: clean build success;
  358/358 tests, zero skips.
- Windows FFmpeg Debug, warnings as errors: clean build success; 377 tests passed
  and the then-conditional Windows symlink fixture skipped. All 20 FFmpeg-dependent
  tests passed.
- Windows FFmpeg Release, warnings as errors: clean build success; 376 tests passed,
  the same symlink fixture skipped, and the process-death fixture was initially
  blocked before launch. Direct rerun of the process-death fixture passed.
- FFmpeg Debug long-timeline test: accelerated 30-minute, two-track file/index
  agreement completed in 94.58 seconds.
- FFmpeg Release long-timeline test: completed in 76.26 seconds.
- QML recovery, startup routing, Studio diagnostics, controller threading, and
  production recording lifecycle tests passed in Debug and Release.
- `git diff --check`: clean.

The old conditional Windows symlink skip was then removed. The replacement creates
an actual NTFS junction without administrator privilege, points `project.db` outside
the package, and verifies rejection. It passed in the final Debug and Release suites,
both of which completed 358/358 with zero skips. macOS/Linux retain the symbolic-link
form. Thus every logical recovery/security case was executed, although the test-only
FFmpeg binaries were not both relinked after this fixture replacement.

## Windows Code Integrity note

This machine enforces Enterprise Code Integrity policy
`{0283ac0f-fff1-49ae-ada1-8a933130cad6}`. It intermittently blocks a newly linked,
unsigned local test executable before `main()`. The failure is identifiable because
the process-death parent receives `-1` instead of the fixture's deliberate exit 73,
and Event 3077 names the blocked executable. Direct rerun of the fixture passed.

The policy was not disabled. After the final production-code verification, a
test-only re-link of the FFmpeg Debug test binary was blocked again; it was not
repeated to avoid user-facing Windows Security popups. The replacement reparse test
was executed in both final non-FFmpeg Debug and Release binaries, and no production
source changed after the successful FFmpeg Debug/Release suites.

## Honest remaining boundary

- R0-03/R0-04 physical Apple capture acceptance remains pending.
- R0-05/R0-06 real-time two-hour thermal, device-clock, hotplug, disk-throughput,
  and force-kill acceptance remains pending on the target macOS hardware.
- Accelerated tests prove arithmetic, state bounds, persistence ordering, and encoded
  media behavior; they do not substitute for a physical oscillator/driver soak.
