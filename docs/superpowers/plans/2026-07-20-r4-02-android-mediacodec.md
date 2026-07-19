# R4-02 Android MediaCodec Completion Plan

**Goal:** Replace Android's unavailable recording/export placeholders with durable MediaCodec/MediaMuxer paths while preserving the existing project and recorder contracts.

**Architecture:** The shared recorder keeps ownership of clocks, queues, two-second segmentation, atomic publication, and recovery metadata. Android supplies only a platform `ITrackSegmentEncoder`; Java owns `MediaCodec`/`MediaMuxer` handles and C++ owns generation, format, and timestamp validation. Android output uses typed MP4 segment extensions rather than disguising MP4 bytes as Matroska.

### Task 1: Typed portable segment container

- [x] Add a typed `SegmentContainer` selector to the encoder port with Matroska as the compatibility default.
- [x] Prove video/audio MP4 paths become `.mp4`/`.m4a` while existing FFmpeg paths remain `.mkv`/`.mka`.
- [x] Thread the selector through `AsyncTrackRecorder` and `DurableSegmentPublisher`.

### Task 2: MediaCodec source-track encoder

- [x] Add a Qt-free generation/timestamp state policy and RED/GREEN tests.
- [x] Add Android C++ video/audio encoder adapters with bounded BGRA and float-PCM conversion.
- [x] Add Java MediaCodec/MediaMuxer ownership, EOS drain, abort cleanup, and error propagation.
- [x] Select the native Android encoder in the existing live-recording orchestration.
- [x] Verify Windows tests, policy tests, both Android ABIs, and emulator cold start.

### Task 3: MediaCodec export path

- [x] Add an Android render job that accepts the existing immutable `RenderRequest` and publishes only after MediaMuxer completion.
- [x] Keep unsupported timeline constructs explicit; never report a successful export with omitted media, audio, titles, captions, or effects.
- [x] Verify cancellation, background interruption, scoped destination publication, both ABIs, and emulator smoke.

### Task 4: Acceptance evidence

- [x] Add an adb/device matrix for screen/camera/microphone segments, reopen/recovery, timeline export, deny/revoke/background, thermal pressure, and arm64 hardware codec selection.
- [x] Record emulator evidence separately; do not substitute it for the physical arm64 gate.
