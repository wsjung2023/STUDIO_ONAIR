# R0-04 Camera and Audio Capture Design

**Status:** Approved for implementation on 2026-07-16. Apple compilation and
physical-device acceptance remain mandatory gates, not assumptions.

## 1. Goal

Add a macOS-first camera, microphone, and system-audio capture foundation that
can run beside R0-03 screen capture. Device arrival/removal and source failure
must be visible, bounded, and isolated: unplugging a camera must not terminate
the application or silently stop the microphone, system audio, or screen.

R0-04 does not encode or persist media. R0-05 consumes the neutral frames and
blocks established here.

## 2. Acceptance

Code-level acceptance:

- enumerate camera and microphone devices with opaque typed IDs;
- independently request and expose camera and microphone permission;
- receive camera frames, microphone blocks, and system-audio blocks on serial
  native callback queues;
- map the first valid sample of every source onto the common project monotonic
  timebase without assuming a zero native epoch;
- run screen, camera, microphone, and system audio concurrently;
- refresh device snapshots on hotplug and stop only a removed active source;
- expose actual camera dimensions, audio peak/RMS levels, block counts, queue
  overruns, and actionable errors in the Studio UI;
- bound every handoff and make any audio overrun visible rather than silently
  dropping it;
- release callbacks and native sessions deterministically during stop/destruction.

Product acceptance additionally requires a macOS machine with real camera and
microphone hardware. It must demonstrate concurrent capture, permission grant
and denial, hotplug, and safe target failure. Windows tests cannot satisfy that
gate.

## 3. Platform APIs and permission boundary

The Apple adapter uses AVFoundation `AVCaptureDevice.DiscoverySession` for
camera/microphone snapshots and the documented connected/disconnected
notifications for hotplug. Camera video uses `AVCaptureVideoDataOutput` and
microphone audio uses `AVCaptureAudioDataOutput`; both delegates run on private
serial dispatch queues.

System audio remains a ScreenCaptureKit responsibility. A dedicated audio-only
`SCStream` captures 48 kHz stereo output independently from the R0-03 screen
stream. Keeping it separate preserves source-separated R0-05 recording and
allows either source to stop without changing the other.

Camera and microphone permission are distinct. The bundle declares
`NSCameraUsageDescription` and `NSMicrophoneUsageDescription`; production
signing must include camera and audio-input entitlements. Permission callbacks
return to the application thread before changing Qt-visible state.

References:

- Apple, AVCaptureDevice and device monitoring:
  https://developer.apple.com/documentation/avfoundation/avcapturedevice
- Apple, capture authorization:
  https://developer.apple.com/documentation/avfoundation/requesting-authorization-to-capture-and-save-media
- Apple, AVCaptureVideoDataOutput late-frame policy:
  https://developer.apple.com/documentation/avfoundation/avcapturevideodataoutput/alwaysdiscardslatevideoframes
- Apple, ordered AVCaptureAudioDataOutput callbacks:
  https://developer.apple.com/documentation/avfoundation/avcaptureaudiodataoutput/setsamplebufferdelegate(_:queue:)
- Apple, ScreenCaptureKit audio output:
  https://developer.apple.com/documentation/screencapturekit/scstreamoutputtype/audio

## 4. Qt-free ports and values

`CaptureDeviceKind` is `Camera` or `Microphone`. `CaptureDeviceInfo` carries a
typed `CaptureDeviceId`, display name, and default-device flag. IDs wrap native
unique IDs but no AVFoundation string or object crosses the adapter boundary.

`IDeviceCaptureBackend` owns four responsibilities that must share the same
native discovery snapshot:

- permission status/request for camera and microphone;
- current device snapshot;
- a change callback that only says the snapshot is stale;
- factories for camera, microphone, and system-audio sources.

Each factory returns an `IDeviceCaptureSource`. Its asynchronous start/stop
completion prevents `AVCaptureSession::startRunning` and native teardown from
blocking the UI. Completion is exactly once; `stopAsync` immediately bars new
sink callbacks and coalesces duplicate stop requests.

Camera sources constructor-inject `IVideoFrameSink`. Audio sources
constructor-inject `IAudioBlockSink`. Both sinks accept immutable neutral media
and terminal `AppError`s only; native types stay inside the adapter.

## 5. Video delivery

Camera output requests BGRA CVPixelBuffers for the existing SDR Rec.709 path.
`alwaysDiscardsLateVideoFrames` remains enabled so camera backpressure cannot
grow memory without bound; AVFoundation's dropped-frame delegate increments a
native-reported drop counter.

For every complete camera sample:

1. validate data readiness, PTS, CVPixelBuffer, format, and dimensions;
2. lazily anchor PTS to `ProjectClock::now()` at the first valid frame;
3. retain the CVPixelBuffer in `VideoFrame::platformHandle`;
4. publish full-surface crop/content metadata to the sink without copying;
5. update received, invalid, dropped, and current-fps diagnostics.

The controller uses a separate one-slot `LatestVideoFrameMailbox` for camera
diagnostics/preview ownership. Replacements are preview replacements, never
reported as camera device drops.

## 6. Audio delivery and levels

The Apple boundary normalizes microphone and system audio to interleaved
float32 while retaining the source sample rate and channel count in every
`AudioBlock`. Format conversion belongs only at this adapter boundary.

Each audio source lazily maps its first valid PTS to project time, then copies
the callback-owned samples into an immutable `AudioBlock`. A fixed-capacity
`BoundedAudioBlockQueue` transfers ownership to the application. `tryPush`
never blocks the native callback. If full, it increments an overrun count and
reports a named backpressure error; it never pretends the block was accepted.
R0-05 may add a short encoder-side wait, but may not weaken this visibility.

`AudioLevelMeter` consumes samples and calculates per-block linear peak and RMS
across all channels. Empty, non-finite, malformed, and inconsistent blocks are
rejected. The UI converts to dBFS with a documented floor of -96 dBFS; silence
is not represented as NaN or negative infinity.

## 7. Application state and hotplug

`DeviceCaptureController` owns independent camera, microphone, and system-audio
slots. Each slot has `Disabled`, `Ready`, `Starting`, `Capturing`, `Stopping`,
or `Error` state plus a generation number. A stale async completion cannot
revive a stopped or replaced source.

On a device-change notification the controller refreshes the full snapshot. If
the selected device still exists, capture continues. If an active selection is
gone, only that slot stops and reports `Camera disconnected` or `Microphone
disconnected`; the other slots and R0-03 `ScreenCaptureController` are untouched.

The QML page provides camera/microphone selectors, independent enable controls,
system-audio enable, permission actions, live camera size/fps, and two audio
meters. Source controls are disabled only while their own slot is transitioning.

## 8. Failure and shutdown rules

- Permission denial names the exact source and tells the user where to grant it.
- Device busy, configuration rejection, malformed native buffers, and native
  session interruption become distinct `AppError` messages.
- Audio queue overrun is visible in both the counter and status; no silent loss.
- Stop bars callbacks before native teardown and completes exactly once.
- Controller destruction invalidates generations, clears hotplug callbacks,
  stops all three sources, and drains/destroys retained media.
- No platform callback captures a raw Qt object without a lifetime guard.

## 9. Verification matrix

Platform-neutral deterministic tests cover device snapshot reconciliation,
hotplug isolation, stale completions, queue capacity/order/ownership, overrun
visibility, audio level math, timestamp anchoring, malformed input, and source
shutdown. QML smoke tests cover controls, permission, and error text.

Windows Debug/Release prove that the lower layers stay Qt-free and all stubs are
usable. macOS Debug/Release must compile the Objective-C++ adapter with warnings
as errors. Real-device acceptance then exercises concurrent capture, permission
changes, camera and microphone unplug, two input formats, and ten minutes of
bounded-memory operation.
