# R0-05 Source-Separated Recording Design

Date: 2026-07-16
Branch: `feat/r0-05-source-recording`
Status: approved for implementation by the existing roadmap and the user's instruction to continue autonomously

## Outcome

R0-05 replaces the fake-only recording path with bounded, asynchronous recording
of independent screen, camera, microphone, system-audio, and optional composite
preview tracks. Each track publishes playable two-second Matroska segments and
keeps the SQLite segment index consistent with the files that were durably
published.

R0-05 does not solve long-run cross-device drift. It preserves every source's
project timestamp and exposes queue/encoder timing so R0-06 can measure and
correct drift without changing the storage contract.

## Fixed constraints

- FFmpeg is fixed to official release 8.1.2. The official release archive used
  during design had SHA-256
  `464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c`.
- FFmpeg is built as dynamic libraries without `--enable-gpl` and without
  `--enable-nonfree`. The exact source archive, configure line, configuration
  string, and license string are retained as build evidence.
- Domain and capture interfaces expose no FFmpeg types. FFmpeg headers are
  confined to `src/ffmpeg_adapter`.
- Encoding, muxing, fsync, rename, and SQLite work never run on the UI thread.
- Every queue is bounded. Video may be dropped with an explicit counter; audio
  queue exhaustion is a terminal recording error.
- Segment boundaries use project timestamps, not wall clock.

References:

- official source release: https://ffmpeg.org/download.html
- official LGPL checklist: https://ffmpeg.org/legal.html
- send/receive encoding contract:
  https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html
- segment/keyframe behavior: https://ffmpeg.org/ffmpeg-formats.html#segment

## Architecture

```text
capture sinks / compositor
          |
          v
  MultiTrackRecordingService       application-owned orchestration
          |
          +-- bounded video mailbox ----+
          +-- bounded audio queue -------+--> worker thread per track
                                           |       |
                                           |       v
                                           |  ITrackSegmentEncoder
                                           |  (FFmpeg adapter)
                                           |       |
                                           v       v
                                 DurableSegmentPublisher
                                      |          |
                                      v          v
                              ISegmentLifecycleSink
                              (project-store adapter)
```

The capture layer only pushes immutable neutral `VideoFrame` and `AudioBlock`
values. The application service owns the recording queues and lifecycle. The
recorder layer owns segment policy and durable publication. A project-store
adapter implements the neutral lifecycle sink; the encoder never calls SQLite
directly.

## Track model and paths

`RecordingTrack` carries a stable `SourceId`, media kind, and role:

| Role | Media | Published path |
|---|---|---|
| screen | video | `media/screen/<source>/segment_000000.mkv` |
| camera | video | `media/camera/<source>/segment_000000.mkv` |
| composite preview | video | `media/preview/<source>/segment_000000.mkv` |
| microphone | audio | `audio/microphone/<source>/segment_000000.mka` |
| system audio | audio | `audio/system/<source>/segment_000000.mka` |

IDs are encoded into safe path components by a dedicated policy; raw device
names and unvalidated paths never become filenames. Segment indices are
zero-based and monotonically increasing per source.

Composite recording is just another optional video track. It consumes the
compositor's output when enabled; R0-05 does not invent a composite frame when
the compositor has none.

## Segment lifecycle and crash ordering

For each segment:

1. Check free space and reserve policy.
2. Send `WRITING` metadata through `ISegmentLifecycleSink::begin`.
3. Open a unique package-local `.tmp/...mkv.part` file.
4. Encode/mux packets. Video encoders force a closed GOP/keyframe at the
   segment start; timestamps inside each segment are rebased to zero.
5. Flush the encoder, write the container trailer, fsync, and close.
6. Atomically rename the `.part` file to its final media path and sync the
   parent directory where the platform supports it.
7. Send the exact final duration and path through
   `ISegmentLifecycleSink::ready`.

Any failure after step 2 sends `failed` exactly once. A failure before final
rename leaves no READY row. A failure after rename but before READY is recovered
from the WRITING row in R0-06; the final file is never overwritten. Incomplete
`.part` quarantine is also completed in R0-06.

This ordering intentionally favors a durable unindexed file over a READY row
whose file does not exist.

## FFmpeg adapter

The adapter probes encoders by name and records the chosen capability:

- macOS video: `h264_videotoolbox`, with realtime mode and software fallback
  allowed only when FFmpeg reports it;
- Windows video: a supported H.264 hardware wrapper when present; otherwise the
  LGPL native `mpeg4` encoder for correctness and CI coverage;
- audio: FFmpeg's native AAC encoder;
- container: Matroska (`.mkv` video, `.mka` audio).

The adapter uses `avcodec_send_frame`/`avcodec_receive_packet`, rescales packet
timestamps to the muxer timebase, and writes with
`av_interleaved_write_frame`. Audio input remains float32 and is converted with
libswresample only when the selected encoder requires a different sample
format. Video conversion is hidden behind `IVideoFrameMapper`; the macOS mapper
locks the retained `CVPixelBuffer` only on the encoder worker and supplies BGRA
planes to libswscale. Preview remains zero-copy.

Each two-second video file is independently decodable because the first frame
is forced to be a keyframe and the encoder is flushed at the boundary. The
segment muxer is not used as a hidden second state machine; Creator Studio owns
the boundary and persistence transaction explicitly.

## Backpressure and threading

- One worker thread owns each track encoder and all of its FFmpeg objects.
- Video input uses a bounded newest-frame queue. On overflow, the displaced
  frame is released and `framesDropped` increments.
- Audio input uses a bounded FIFO sized in frames. It never silently drops; an
  overflow latches a terminal error visible to the service and UI.
- Start and stop are asynchronous service operations. Stop closes input,
  drains accepted data, finalizes the trailing segment, and completes exactly
  once.
- No worker callback directly touches QML. The application controller marshals
  state and metrics onto its Qt thread.

## Disk-space policy

`DiskSpaceMonitor` wraps `std::filesystem::space` behind an injectable probe.
Before opening each segment and periodically while writing, it verifies:

- free space is above a configurable hard reserve (default 1 GiB); and
- free space is above the hard reserve plus the current track's conservative
  next-segment estimate.

The service exposes available bytes, reserve bytes, and the last check error.
Crossing the limit stops accepting new media and finalizes already accepted
data; it is not reported as a successful stop.

## Persistence integration

`ProjectSegmentLifecycleSink` belongs to the application boundary. It carries
the opened package path and session ID and invokes the existing
`IProjectPackageStore::beginSegment`, `markSegmentReady`, and
`markSegmentFailed` methods on the recording worker. It serializes operations
per session and never lets a capture adapter see a database object.

At final stop, `RecordingSession` contains exactly the READY segments already
published through the lifecycle sink. `completeRecording` remains the final
session transaction and must agree with the per-segment rows.

## Verification

Deterministic tests use injected encoders, filesystem probes, and lifecycle
sinks to cover:

- exact two-second boundaries and trailing partial segments;
- independent paths and indices for every role;
- no READY-before-file ordering;
- encoder, disk, rename, and persistence failures;
- bounded video dropping and terminal audio overflow;
- exactly-once async stop under concurrent calls;
- DB rows matching the final media tree.

FFmpeg-enabled integration tests generate synthetic BGRA video and float32
audio, reopen every file through libavformat/libavcodec, decode at least one
packet/frame, and compare durations and stream metadata. A timestamp-accelerated
30-minute run validates all track segment indices without waiting 30 wall-clock
minutes; real-time soak remains a product acceptance run.

## Honest acceptance boundary

R0-05 code completion requires all platform-neutral tests and an FFmpeg-enabled
playback/index integration run. macOS VideoToolbox/CVPixelBuffer behavior still
requires a Mac. A timestamp-accelerated test is not evidence of thermal or disk
throughput stability, so the final record distinguishes structural 30-minute
coverage from a physical real-time soak.
