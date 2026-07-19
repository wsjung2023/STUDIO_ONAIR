# R0-04 Camera and Audio Capture Implementation Plan

> **Execution:** Use `superpowers:executing-plans`, test-driven development for
> every behavior, and verification-before-completion before any success claim.

**Goal:** Deliver independent, hotplug-safe camera, microphone, and system-audio
capture beside R0-03 screen capture, with bounded handoff and visible levels.

**Architecture:** Qt-free device/source ports feed a latest-frame camera mailbox
and fixed-capacity audio queues. A Qt application controller owns independent
source state machines. Apple-only AVFoundation and ScreenCaptureKit adapters
convert native sample buffers at the boundary.

## Constraints

- Preserve the three user-owned untracked PNG files.
- Never block a native media callback or the Qt UI thread.
- No AVFoundation, ScreenCaptureKit, CoreMedia, or Qt types in neutral ports.
- No wall-clock media timestamps and no first-source zero-epoch assumption.
- No unbounded queue or silent audio loss.
- One source failure must not stop unrelated sources or the application.
- Windows evidence cannot be presented as Apple compile/device evidence.

## Tasks

### Task 1: Device values, permissions, and backend ports

- [ ] Write failing value and compile-time port contract tests.
- [ ] Add `CaptureDeviceId`, kind/info/permission values,
  `IDeviceCaptureBackend`, `IDeviceCaptureSource`, and `IAudioBlockSink`.
- [ ] Add deterministic unsupported backend and fakes.
- [ ] Prove `cs_capture` stays transitively Qt-free; commit.

### Task 2: Bounded audio handoff and level math

- [ ] Write failing tests for FIFO order, capacity, ownership release, clear,
  overrun count, concurrent producer/consumer, peak, RMS, silence, and malformed
  samples.
- [ ] Implement `BoundedAudioBlockQueue` and `AudioLevelMeter` without sleeps.
- [ ] Run focused Qt-free tests and commit.

### Task 3: Camera and audio timestamp assemblers

- [ ] Write failing tests for lazy first-valid anchoring, rational PTS,
  monotonicity, full camera crop, invalid geometry/format, audio metadata, and
  immutable sample ownership.
- [ ] Implement platform-neutral assemblers around `CaptureTimestampMapper`.
- [ ] Run capture tests and commit.

### Task 4: DeviceCaptureController state machines

- [ ] Write deterministic failing tests for discovery, permission, independent
  start/stop, simultaneous sources, stale completion, hotplug isolation,
  terminal error, overrun, drain/levels, and destruction.
- [ ] Implement independent slot generations and UI-thread callback marshal.
- [ ] Expose selectors, state, statistics, and actionable status; commit.

### Task 5: Studio device controls and diagnostics

- [ ] Extend QML fakes and smoke tests first.
- [ ] Add camera/microphone selectors, source toggles, permission actions,
  system-audio toggle, meters, and per-source error text.
- [ ] Keep non-macOS controls explicitly unsupported, never fake-live; commit.

### Task 6: AVFoundation discovery, permission, and hotplug

- [ ] Add pure mapping seams/tests where native-free testing is possible.
- [ ] Implement Apple-only device snapshots, default selection, distinct
  permissions, connected/disconnected monitoring, usage descriptions, and
  production entitlement template.
- [ ] Keep callbacks serial and stop monitoring before backend destruction.
- [ ] Verify Windows does not compile/link Apple sources; commit.

### Task 7: AVFoundation camera and microphone sources

- [ ] Implement camera BGRA CVPixelBuffer delivery, reported late-frame drops,
  lazy timestamp mapping, retained handles, and async session lifecycle.
- [ ] Implement microphone float32 interleaved conversion with source metadata,
  serial delivery, lazy timestamp mapping, and named interruption errors.
- [ ] Use exact callback barriers and coalesced stop completion; commit.

### Task 8: ScreenCaptureKit system-audio source

- [ ] Implement an independent audio-only SCStream at 48 kHz stereo with current
  process audio excluded.
- [ ] Convert AudioBufferList data to neutral float32 interleaved blocks and
  expose native failure/stop exactly once.
- [ ] Prove screen and system-audio ownership are independent; commit.

### Task 9: Cross-platform review and verification

- [ ] Run focused tests, full Debug/Release builds and tests, `git diff --check`,
  Qt-free graph checks, and QML smoke.
- [ ] Request independent code review and close all Critical/Important findings.
- [ ] Record Windows policy skips/failures and the Apple verification gap exactly.

### Task 10: macOS compile and device acceptance

- [ ] Run macOS Debug and Release builds/tests with warnings as errors.
- [ ] Verify permission grant/denial for camera and microphone.
- [ ] Capture screen + camera + microphone + system audio concurrently.
- [ ] Unplug active camera and microphone independently; app stays alive and
  unrelated sources continue.
- [ ] Run ten-minute capture with bounded queues and stable memory.
- [ ] Record actual formats, rates, drops, overruns, errors, and evidence.

Do not mark R0-04 product acceptance complete until Task 10 is evidenced.
