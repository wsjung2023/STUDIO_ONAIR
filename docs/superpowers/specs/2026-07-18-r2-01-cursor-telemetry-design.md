# R2-01 Cursor and Click Telemetry Design

## R2 stage map

R2 is split into seven product checkpoints so the numbering remains explicit:

1. **R2-01** cursor coordinates and click-event recording;
2. **R2-02** click highlighting and replaceable/hidden captured cursor;
3. **R2-03** automatic zoom candidates and editable zoom regions;
4. **R2-04** local whisper.cpp transcription with word/sentence timestamps;
5. **R2-05** transcript editing, non-destructive text cuts, silence and filler
   suggestions;
6. **R2-06** RNNoise, optional DeepFilterNet, compressor/limiter, and loudness
   normalization;
7. **R2-07** complete local-AI, approval/cancellation, failure-atomicity, and
   long-form physical verification.

This document covers R2-01 only. It does not remove or downgrade any later R2
checkpoint. Implementation starts only after R1-06 Export and R1-07 final R1
verification are complete, and then integrates into the verified R1 base.

## Goal and user-visible result

Every Studio recording can optionally retain a source-separated stream of
physical cursor coordinates and mouse-button transitions on the same monotonic
project timebase as screen, camera, microphone, system audio, scene switches,
and markers.

The cursor stream is durable project data, not baked only into the video. It is
the source for later click highlights, cursor replacement/hiding, and automatic
zoom. A telemetry failure never corrupts or discards valid video/audio. The HUD
shows whether cursor telemetry is recording, its event count, any coalesced move
count, and a short failure message.

R2-01 captures no key names, typed text, clipboard contents, target UI element,
window title, application name, device serial number, or user identifier. It
starts only with an accepted recording session and unregisters on stop,
preparation failure, controller destruction, or application shutdown.

## Considered approaches

### Selected: application-owned Raw Input adapter

On Windows, the executable owns one hidden message-only window and registers the
generic mouse usage with `RegisterRawInputDevices` using `RIDEV_INPUTSINK` and
`RIDEV_DEVNOTIFY`. `WM_INPUT` supplies button transitions and movement activity;
the adapter samples `GetPhysicalCursorPos` for authoritative physical desktop
coordinates. This avoids a global `WH_MOUSE_LL` hook, injected DLL, driver,
administrator privilege, network service, or third-party runtime.

Microsoft documents that only one window per raw-input device class can be
registered in a process. Therefore registration belongs to the Creator Studio
executable composition root, not a reusable library that silently steals another
component's registration. The adapter is inactive outside a recording session.

### Rejected: low-level global mouse hook

`WH_MOUSE_LL` exposes similar transitions but increases security-product
sensitivity, requires more defensive timeout behavior, and is unnecessary for
the required data. It is not used.

### Rejected: frame polling only

Polling cursor position at frame rate is simple but can miss short click
transitions and couples telemetry quality to video delivery. Cursor data remains
an independent source even when video frames are delayed or dropped.

## R2-01 architecture

The feature uses five isolated units:

1. `CursorTelemetryEvent` and geometry value types in a Qt-free telemetry
   library;
2. `ICursorTelemetrySource`, implemented by the Windows Raw Input adapter and a
   deterministic fake;
3. `CursorTelemetryRecorder`, a bounded worker that normalizes, coalesces, and
   durably writes events;
4. `ICursorTelemetryStore` and SQLite metadata for atomic stream state and crash
   recovery;
5. `CursorTelemetryBinding`, which attaches the optional telemetry source to the
   existing accepted recording lifecycle and publishes HUD diagnostics.

No existing capture source is made responsible for telemetry. Screen preview,
screen encoding, and cursor events can fail, stop, and be tested independently.
The live recording engine receives a telemetry binding alongside its existing
video/audio bindings, preserving the current source-separated architecture.

## Value model and invariants

`CursorTelemetryEvent` contains:

- contiguous unsigned sequence number;
- monotonic project timestamp in nanoseconds;
- event type: `move`, `button_down`, or `button_up`;
- button for transitions: `left`, `right`, `middle`, `x1`, or `x2`;
- signed physical virtual-desktop X/Y pixels;
- target-relative X/Y in integer parts-per-million from 0 to 1,000,000 when the
  cursor is inside the recorded target;
- `inside_target` and geometry generation.

Floating-point coordinates are not persisted. Integer parts-per-million give
canonical cross-platform JSON and stable zoom math. Button is absent for move
events and mandatory for button transitions. Timestamps never move backward,
sequences never repeat or skip, and button-up cannot be emitted without a known
down state unless the stream begins while that button is already held; that
initial condition is represented in the header.

`CursorTargetGeometry` contains a generation number, physical desktop rectangle,
capture raster, and DPI-awareness mode. A geometry record is written before the
first event and whenever the target moves, resizes, changes display scale, or is
replaced. Events reference the latest committed geometry generation. A cursor
outside the target retains desktop coordinates but has no relative coordinates.

The first implementation supports one cursor stream for each actively recorded
screen source. Camera and audio sources never receive cursor streams.

## Timestamp and queue behavior

The native callback stamps arrival with the injected steady clock. The existing
capture timestamp mapping converts it to the recording's monotonic project
timebase. A timestamp earlier than the prior event clamps to the prior timestamp
and increments a correction diagnostic; it is never silently persisted out of
order.

Movement is sampled at no more than 120 events per second per source. Multiple
move notifications inside one sample interval replace the pending move with the
latest position. Button down/up transitions are placed in a separate bounded
queue and are never replaced by movement. The recorder drains transitions in
timestamp/sequence order with the latest eligible move.

The transition queue holds at least 4,096 events. Exhaustion marks that telemetry
stream failed because a click cannot be truthfully reconstructed. The primary
recording continues and the HUD reports the failure. Memory remains bounded; no
producer callback performs file I/O, SQLite work, allocation proportional to
recording duration, or GUI dispatch.

## Durable file format

Each stream is written below the validated project package as:

```text
telemetry/cursor/<session-id>-<screen-source-id>.ndjson.part
telemetry/cursor/<session-id>-<screen-source-id>.ndjson
```

The final relative path is stored in SQLite. IDs are validated domain IDs and
cannot contain separators. Package containment and Windows reparse/hard-link
checks follow the same rules as recorded media.

The UTF-8 NDJSON stream uses canonical key order and one record per newline:

- one header containing schema version 1, session/source IDs, start timestamp,
  initial held-button mask, and privacy declaration;
- geometry records;
- cursor event records;
- one footer containing final sequence, event counts, coalesced-move count,
  timestamp corrections, stop timestamp, and terminal status.

No unbounded line is accepted. The header, geometry, event, and footer schemas
have exact field allowlists, integer bounds, and record-size limits. Finalization
flushes and closes the `.part`, calculates SHA-256 and byte size, atomically
renames on the same volume, and only then marks the SQLite row ready.

## Persistence and recovery

Migration 005 follows the R1-06 render-job migration and adds
`telemetry_streams` with:

- stream, project, recording-session, and source identities;
- kind constrained to `CURSOR_V1`;
- package-relative final and part paths;
- state constrained to `WRITING`, `READY`, or `FAILED`;
- start/end timestamps, event count, byte size, SHA-256, and failure diagnostic;
- unique `(session_id, source_id, kind)` identity.

The transaction that begins a cursor stream requires an existing active
recording session. `WRITING -> READY|FAILED` are the only state changes; terminal
rows are immutable except idempotent identical requests.

Recovery validates package identity before touching files:

- a valid final file whose hash/size/footer match a `WRITING` row is completed
  idempotently;
- a `.part` is scanned only through its last complete newline; valid header,
  geometry, and contiguous events are closed with a recovered footer, durably
  renamed, and marked ready;
- malformed, redirected, hard-linked, oversized, noncontiguous, or identity-
  changed data is quarantined and the row is failed;
- unrelated files and ready streams are never moved or rewritten.

R1-06 owns migration 004. R2-01 must retain migration 005 when it is implemented
after the R1 completion gate.

## Recording lifecycle and UI

The ordered lifecycle is:

1. project persistence accepts and commits the recording session;
2. Studio freezes the active source snapshot and screen geometries;
3. cursor stream rows and `.part` writers are prepared;
4. the platform telemetry source registers, then video/audio encoding starts;
5. stop disables native input first, drains cursor queues, finalizes telemetry,
   and then completes the primary recording session;
6. reconciliation exposes ready telemetry to later R2 processors.

Preparation failure before video start aborts all prepared cursor rows. A native
telemetry failure after video starts fails only cursor telemetry and does not
abort useful media. Controller destruction unregisters Raw Input, drains the
worker, and closes the file before releasing stores. No running thread or native
window survives its owner.

Studio HUD additions are model-driven and accessible:

- `Cursor data: On`, `Unavailable`, or `Failed`;
- captured event count and coalesced move count;
- a concise warning that video/audio recording continues when telemetry fails.

The project offers an explicit cursor-data recording toggle, default on for
screen sources. The toggle cannot change during recording. It controls only
telemetry; it does not yet hide the cursor baked into screen video, which is
R2-02.

## Platform boundary

Windows 11 x64 is the physical R2-01 target. The Windows adapter uses only
User32 and standard C++/Win32 APIs. The Qt-free domain, recorder, store, fake,
and lifecycle contracts compile on all supported platforms.

macOS receives an explicit unavailable adapter until native Input Monitoring or
Accessibility permission behavior is separately designed and physically tested.
The product must say unavailable; it may not fake cursor events or silently claim
cross-platform parity.

## Verification gates

R2-01 is not complete without all of the following:

- value tests for every event, button, geometry, coordinate, timestamp, and
  canonical serialization invariant;
- queue tests proving move coalescing, transition preservation, bounded memory,
  ordering, overflow failure, and producer nonblocking behavior;
- migration checksum, constraint, idempotency, Unicode path, failure injection,
  hard-link/reparse, and exact crash-recovery tests;
- native adapter tests through an injected Win32 seam using synthetic
  `RAWMOUSE` records, including every button flag, device changes, registration
  failure, cursor-position failure, and deterministic unregister;
- recording lifecycle tests for preparation, start, stop, native failure,
  store failure, recovery, retry, and repeated controller destruction;
- an accelerated 30-minute stream with at least 216,000 move samples and 10,000
  button transitions proving bounded memory, file size, parse time, and exact
  counts;
- QML accessibility, action/state parity, small-window layout, and honest HUD
  failure tests;
- Windows physical application probe confirming the Raw Input adapter can
  register and unregister without administrator rights or a security dialog;
- complete warning-as-error build, sequential and parallel CTest, app probe,
  source/link/runtime audit, and independent review.

No physical test moves or clicks the user's pointer. Native message parsing is
tested through the Win32 seam; the application probe verifies registration and
cleanup only.

## Authoritative Windows references

- Raw Input registration and single-window ownership:
  https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerrawinputdevices
- `RAWINPUT` retrieval:
  https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-rawinput
- `RAWMOUSE` movement and button flags:
  https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-rawmouse
- Physical cursor position:
  https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getphysicalcursorpos
