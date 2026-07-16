# R0-03 macOS Screen Capture Design

**Date:** 2026-07-16  
**Status:** Approved for implementation  
**Target branch:** `feat/r0-03-native-capture`

## 1. Goal and honest acceptance boundary

R0-03 replaces the shipped test-pattern preview on macOS with a real
ScreenCaptureKit display/window source. It provides target discovery, permission
guidance, pushed video frames, monotonic project timestamps, a bounded preview
path, dynamic-size handling, and a visible terminal-source error.

R0-03 is complete only when all of the following evidence exists:

1. Qt-free unit and contract tests pass on Windows and macOS.
2. macOS Debug and Release compile with warnings as errors.
3. A macOS 13+ machine receives complete ScreenCaptureKit frames.
4. A 1920x1080 display previews at 60 fps without an unbounded queue.
5. Resizing a captured window updates the displayed geometry without restarting
   the application.
6. Closing the selected window produces a visible recoverable error and releases
   the stream.

The current development machine is Windows and the repository has no remote CI.
Therefore platform-neutral code and compile-gated Objective-C++ can be developed
here, but items 2-6 must remain `UNVERIFIED` until macOS evidence is recorded.
No synthetic test result substitutes for that evidence.

## 2. Scope

In scope:

- screen-recording permission preflight and explicit request;
- enumeration of displays and on-screen windows;
- selecting one target before stream creation;
- ScreenCaptureKit video output through an OS callback queue;
- accepting only complete frames with a valid image buffer and timestamp;
- converting CoreMedia rational presentation time to project monotonic time;
- retaining the underlying `CVPixelBuffer`/`IOSurface` through type-erased frame
  ownership;
- a one-slot latest-frame mailbox for preview backpressure;
- visible permission, start, stop, and target-ended state in QML;
- SDR BGRA capture for the first implementation;
- diagnostics for received, dropped, invalid, and late preview frames.

Out of scope:

- camera, microphone, and system audio (R0-04);
- FFmpeg encoding and durable media segments (R0-05);
- multi-clock drift correction (R0-06);
- Windows.Graphics.Capture;
- HDR and wide-gamut colour management;
- region capture and cursor telemetry;
- a CPU-copy preview presented as the production 1080p60 path.

## 3. Chosen architecture

```text
SCShareableContent
        |
        v
IScreenCaptureDiscovery ----> ScreenCaptureController ----> QML target model
                                      |
                                      v
                            MacScreenCaptureSource
                                      |
                          capture callback queue
                                      v
                              IVideoFrameSink
                                      |
                                      v
                           LatestVideoFrameMailbox (1)
                                      |
                         Qt scene-graph render thread
                                      v
                              ScreenPreviewItem
```

The application never calls ScreenCaptureKit directly. Platform adapters depend
on Qt-free capture/media contracts. The frame callback never invokes QML and
never blocks on the render thread.

### 3.1 Push delivery contract

`IVideoFrameSink` is constructor-injected into a push source. This keeps
`ICaptureSource` focused on identity/lifecycle and makes sink lifetime explicit.
Callbacks for a source are serialized. `IScreenCaptureSource::stopAsync()`
immediately bars further sink calls, coalesces duplicate requests, and invokes
every completion exactly once after ScreenCaptureKit reports native teardown.
The controller remains in `Stopping` until that completion and surfaces the
native stop error without blocking the UI thread.

The sink has two operations:

- `onVideoFrame(VideoFrame)` for a valid frame;
- `onCaptureError(AppError)` for an asynchronous terminal failure.

The callback is `noexcept`; adapter exceptions are caught and translated before
crossing the port. A terminal error is delivered at most once.

### 3.2 Preview backpressure

`LatestVideoFrameMailbox` owns at most one pending frame. Publishing replaces
the previous pending frame and increments `replacedFrames`. Taking consumes the
slot. This implements the architecture's latest-frame preview policy and makes
memory use independent of UI speed.

The mailbox is Qt-free and thread-safe. It never waits for the consumer. It
retains the platform handle through `VideoFrame::platformHandle` until replacement
or consumption.

R0-05 will add a separate bounded recording queue; recording must not consume the
preview slot and preview replacement must not count as a capture-device drop.

## 4. Target and permission model

`ScreenCaptureTarget` contains:

- typed `CaptureTargetId`;
- `ScreenCaptureTargetKind::{Display, Window}`;
- display name and optional owning-application name;
- current pixel width and height. Window point bounds are converted using the
  scale of the display containing the largest part of the window.

IDs are opaque session-scoped values generated by the adapter (`display:<id>` or
`window:<id>`). They are not persisted as stable hardware identity.

`IScreenCaptureDiscovery::enumerate()` is asynchronous because
`SCShareableContent` is asynchronous. It returns a complete immutable vector or
one `AppError`; partial lists are not published.

`IScreenCapturePermission` exposes `status()` and `request()`. Status is
`Unknown`, `Granted`, or `Denied`. The UI explains why recording permission is
needed before triggering the OS prompt. Denial leaves the app usable and offers
instructions to open System Settings; it never loops the native prompt.

## 5. Timestamp mapping

ScreenCaptureKit supplies a `CMSampleBuffer` presentation timestamp as an exact
`CMTime { value, timescale }`. Floating point is forbidden.

`CaptureTimestampMapper` anchors the first accepted native timestamp to one
`ProjectClock::now()` sample:

```text
project = project_anchor + (native - native_anchor) * 1,000,000,000 / timescale
```

The mapper:

- rejects non-positive timescales;
- performs quotient/remainder conversion with checked 64-bit arithmetic;
- rejects a native timestamp before the anchor;
- accepts equal consecutive timestamps but reports them to diagnostics;
- never reads wall-clock time;
- is reset for every stream start.

This provides a correct one-source project timeline now. R0-06's
`ClockCoordinator` will compare independent source clocks and apply offsets/drift
policy without replacing the exact conversion contract.

## 6. ScreenCaptureKit adapter

The Objective-C++ adapter is compiled only on Apple and links
`ScreenCaptureKit`, `CoreMedia`, `CoreVideo`, `CoreGraphics`, `IOSurface`, and
`Foundation`.

Discovery uses `SCShareableContent` and excludes the current process's own
windows. The initial deployment target remains macOS 13.0; APIs introduced after
13 are guarded and are not required for the base path.

Stream configuration:

- target width/height from `CaptureConfig`, clamped to the selected content;
- `minimumFrameInterval` from the exact configured frame rate;
- BGRA pixel format and SDR colour space;
- queue depth 3 (bounded and below Apple's documented maximum guidance);
- cursor visible for screen/tutorial capture;
- video output only in R0-03.

For each sample buffer:

1. inspect `SCStreamFrameInfoStatus` and accept only `SCFrameStatusComplete`;
2. require a valid `CVPixelBuffer` image buffer and valid PTS;
3. map PTS through `CaptureTimestampMapper`;
4. read `contentRect`, `contentScale`, and `scaleFactor` from every complete
   frame, convert all four rect edges from points to IOSurface pixels with
   `scaleFactor`, and validate the crop against the IOSurface;
5. carry output-surface size, visible crop, and native content size separately;
6. retain the buffer in `platformHandle` with a matching release deleter;
7. publish without copying pixel bytes.

`stream:didStopWithError:` translates user stop/target removal/permission and
unknown native errors into product errors. Native CoreMedia dropped-frame
attachments, normal `Idle`/`Blank` states, malformed frames, and preview
replacements use distinct counters; the UI labels drops as native-reported
rather than claiming it can infer frames for which no sample arrived.

## 7. Preview integration

`ScreenPreviewItem` is a Qt Quick item whose scene-graph resources are created and
destroyed only on the render thread. The macOS renderer imports the retained
IOSurface into a Metal texture and displays the newest mailbox frame. The item
crops to the per-frame visible content rect before aspect-fit and uses native
content dimensions derived from `contentScale`, so Retina changes and window
resize metadata do not stretch or letterbox the full fixed output surface.

If zero-copy import is unavailable, the controller reports a named fallback
state. A CPU upload may exist for diagnostics, but it cannot satisfy the 1080p60
acceptance item until measured on the target machine.

The QML Studio page shows:

- permission/action state;
- display/window selector;
- start/stop preview control;
- actual width, height, fps, and dropped/ignored/invalid/replaced counters;
- explicit target-ended or permission error text.

## 8. State machine and lifetime

```text
Idle -> CheckingPermission -> Discovering -> Ready -> Starting -> Previewing
 ^             |                |           |          |             |
 |             +----Denied------+-----------+----------+----Error----+
 +------------------------------Stopping <--------------+
```

Commands that do not match the state are rejected with `InvalidState` and do not
partially mutate the stream. Start retains the target, sink, delegate, output
queue, and stream until stop completes. Destruction calls the same stop path and
drains callbacks before releasing the sink.

## 9. Test and evidence strategy

Windows-testable, sleep-free tests cover:

- target value validation and typed IDs;
- exact timestamp conversion including 60/1 and 60000/1001, invalid scale,
  backward time, and overflow;
- one-slot replacement/ownership and concurrent producer-consumer safety;
- push-source callback ordering and stop barrier through a manual fake;
- controller permission/discovery/start/error transitions with deterministic
  fake services;
- QML contract and target-ended error visibility.

macOS tests additionally cover:

- permission translation and `SCShareableContent` conversion;
- Objective-C++ compile/link in Debug and Release;
- instrumented 1080p60 preview, dynamic resize, target close, and resource
  release. Measurements record duration, received/replaced/dropped counts,
  achieved fps, preview P95 latency, and memory slope.

## 10. Primary references

- Apple, ScreenCaptureKit: <https://developer.apple.com/documentation/screencapturekit>
- Apple sample, Capturing screen content in macOS:
  <https://github.com/Fidetro/CapturingScreenContentInMacOS>
- Apple, `SCStreamFrameInfo`:
  <https://developer.apple.com/documentation/screencapturekit/scstreamframeinfo>
- Apple, `CMSampleBufferGetPresentationTimeStamp`:
  <https://developer.apple.com/documentation/coremedia/cmsamplebuffergetpresentationtimestamp(_:)> 
- Qt, `QQuickItem::updatePaintNode`:
  <https://doc.qt.io/qt-6/qquickitem.html#updatePaintNode>
