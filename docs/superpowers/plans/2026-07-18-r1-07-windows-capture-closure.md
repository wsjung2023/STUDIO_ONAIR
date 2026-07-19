# R1-07 Windows Physical Capture Closure Plan

> R2 remains blocked until this plan and the 30-minute product gate pass.

## Goal

Replace the shipping Windows `Unsupported` capture adapters with real display,
camera, microphone, and system-audio capture, then perform the required
30-minute record-edit-reopen-export synchronization run.

## Existing audited boundary

- Reuse the pinned dynamic FFmpeg 8.1.2 build already shipped by the product.
- Use its `gdigrab` display input and `dshow` camera/microphone input.
- Use native Windows WASAPI shared-loopback for system audio because the pinned
  FFmpeg build intentionally has no WASAPI input device.
- Do not add a subprocess, GPL/nonfree codec, cloud service, or synthetic
  capture fallback.

## Task 1: Windows discovery and display capture

1. Add failing contract/physical tests for stable per-scan target IDs, display
   geometry, real BGRA frames, monotonic timestamps, stop barriers, and errors.
2. Implement a Windows screen backend in the FFmpeg adapter using `gdigrab`.
3. Wire it into the Windows shipping composition and verify visible preview and
   recording fanout without blocking the GUI thread.

## Task 2: Camera and microphone capture

1. Add failing tests for physical DirectShow device enumeration and typed IDs.
2. Decode camera frames to owned BGRA surfaces and microphone samples to
   interleaved float PCM through the existing neutral assemblers.
3. Prove hot stop, repeated creation/destruction, and exact error propagation.

## Task 3: System-audio loopback

1. Add tests around injected WASAPI packet conversion and timestamp handling.
2. Implement default-render-endpoint shared loopback with float32 output,
   silence flags, device-position/QPC timestamps, and asynchronous stop.
3. Verify nonzero physical PCM while known system audio is playing.

## Task 4: Shipping integration and preflight

1. Replace only the Windows `Unsupported` backends in `main.cpp`; retain the
   explicit unsupported fallback on other unimplemented platforms.
2. Run the full build, controller/QML tests, device physical tests, hidden app
   responsiveness probe, and import/runtime audit.
3. Record exact selected device identities and preflight evidence.

## Task 5: 30-minute product closure

1. Record a continuous 30-minute screen lesson with camera, microphone, system
   audio, scene transition, marker, and pause/resume through the shipping app.
2. Reopen, remove multiple mistake ranges, ripple delete, move/crop PIP, adjust
   gain/fades, add Unicode title/captions, save, close, and reopen exactly.
3. Cancel one export, retry, and publish the final H.264/AAC MP4.
4. Audit packet monotonicity, representative frames/PCM, start/end A/V drift,
   duration, edits, overlays, database state, partial cleanup, thread cleanup,
   Windows dialogs, and complete sequential/parallel CTest.
5. Fix every observed defect test-first. Only then mark R1-07 complete and
   integrate R1 before starting R2.
