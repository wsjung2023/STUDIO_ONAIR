# R0-03 macOS Screen Capture Implementation Plan

> **Execution:** Use `superpowers:executing-plans`, TDD for every behavior change,
> and `superpowers:verification-before-completion` before any completion claim.

**Goal:** Deliver a bounded, timestamp-correct ScreenCaptureKit preview path with
target discovery, permission guidance, dynamic geometry, and visible terminal
errors while preserving Qt-free lower layers.

**Architecture:** Exact native timestamps and immutable GPU-backed `VideoFrame`s
cross a constructor-injected sink into a one-slot latest-frame mailbox. A Qt
application controller owns state and target selection; a render-thread preview
item consumes the mailbox. ScreenCaptureKit and Metal remain Apple-only adapters.

## Global constraints

- No sleep-based tests or unbounded frame queues.
- No ScreenCaptureKit/CoreMedia/Qt types cross capture/media ports.
- No wall-clock time for media timestamps.
- No pixel readback in the target preview path.
- `stop()` is idempotent and forms a callback barrier.
- All native failures become `AppError`; async errors reach the visible UI.
- Windows Debug/Release remain green with `/WX` after every task.
- macOS and 1080p60 acceptance remain unverified until real evidence exists.
- Preserve the three user-owned untracked PNG files.

## Tasks

### Task 1: Exact capture timestamp mapper

- [ ] Add failing tests for first-frame anchoring, rational conversion,
  60000/1001 cadence, invalid scale, backward time, and overflow.
- [ ] Add `NativeTimestamp` and `CaptureTimestampMapper` to `cs_capture`.
- [ ] Run focused tests and commit.

### Task 2: Screen target and discovery/permission ports

- [ ] Add failing value/port contract tests.
- [ ] Add typed `CaptureTargetId`, target kind/value, permission state,
  `IScreenCaptureDiscovery`, and `IScreenCapturePermission`.
- [ ] Prove lower layers still link no Qt and commit.

### Task 3: Push sink and bounded preview mailbox

- [ ] Add failing tests for frame/error delivery, one-slot replacement,
  retained-handle destruction, and deterministic concurrent access.
- [ ] Add `IVideoFrameSink` and `LatestVideoFrameMailbox`.
- [ ] Add a deterministic manual push fake for application tests.
- [ ] Run Qt-free tests and commit.

### Task 4: Application screen-capture controller

- [ ] Add deterministic failing tests for permission, discovery, selection,
  start/stop, stale completion, terminal error, and destruction.
- [ ] Add `IScreenCaptureSourceFactory` and `ScreenCaptureController`.
- [ ] Keep all callbacks queued onto the controller/UI thread.
- [ ] Publish target model, state, actual geometry, stats, and user-actionable
  status without exposing native types.
- [ ] Run app tests and commit.

### Task 5: QML selection and error surface

- [ ] Extend QML smoke fakes/tests first.
- [ ] Replace the unconditional test pattern on macOS composition with a preview
  surface and target/permission controls; keep a clearly labelled fake fallback
  on non-macOS development builds.
- [ ] Show target-ended errors and live counters.
- [ ] Run QML tests and commit.

### Task 6: ScreenCaptureKit discovery and permission adapters

- [ ] Add adapter conversion tests where pure conversion can remain Qt-free.
- [ ] Add Apple-only Objective-C++ target and framework links.
- [ ] Implement permission preflight/request and async shareable-content mapping.
- [ ] Add bundle usage description and macOS 13 availability guards.
- [ ] Verify Windows builds do not compile or link Apple sources; commit.

### Task 7: ScreenCaptureKit stream and timestamp delivery

- [ ] Add mapper/sink adapter seam tests before implementation.
- [ ] Implement complete-frame filtering, pixel-buffer retention, dynamic
  dimensions, exact PTS mapping, counters, terminal-error translation, and the
  stop callback barrier.
- [ ] Use queue depth 3 and exact configured minimum frame interval.
- [ ] Commit with Windows regression evidence and explicit macOS compile status.

### Task 8: Metal/Qt Quick preview adapter

- [ ] Add render-resource lifetime and frame-replacement tests around the
  platform-neutral seam.
- [ ] Implement Apple-only IOSurface/Metal import on the scene-graph render
  thread with aspect fit and dynamic dimensions.
- [ ] Report, never hide, any CPU fallback.
- [ ] Commit without claiming performance until measured.

### Task 9: macOS build and device acceptance

- [ ] Run macOS Debug configure/build/test with warnings as errors.
- [ ] Run macOS Release configure/build/test.
- [ ] Exercise permission grant/denial, display/window enumeration, start/stop,
  resize, target close, and repeated resource release.
- [ ] Measure 1080p60 preview counters, P95 latency, and memory slope.
- [ ] Fix failures and repeat until all R0-03 acceptance items have evidence.

### Task 10: Review and closeout

- [ ] Run diff/static checks and Windows Debug/Release full suites.
- [ ] Request code review and resolve all Critical/Important findings.
- [ ] Update README/tests README/progress with exact evidence and limitations.
- [ ] Perform final verification after documentation commit.

