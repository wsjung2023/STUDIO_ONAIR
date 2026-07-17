# R1-06 Revision-Frozen H.264 MP4 Export Design

## Status and boundary

R1-06 is the Export stage in the canonical seven-stage R1 roadmap. R1-05
Studio is complete. R1-07 remains the final 30-minute capture-edit-export and
A/V synchronization product acceptance. There is no R1-08 stage.

R1-06 delivers production-shaped H.264 MP4 export without reducing the product
to an MVP:

- revision-frozen export of the complete Editor graph;
- 1920x1080 and 3840x2160 progressive presets;
- actual hardware encoder preflight and bounded software fallback;
- AAC audio, progress, cancellation, failure recovery, and atomic publication;
- persistent render-job diagnostics and crash recovery;
- accessible Export UI and short physical output acceptance on Windows x64.

The full 30-minute user journey remains R1-07 so the export implementation can
first prove its own correctness, failure atomicity, and bounded lifecycle.

## Chosen approach

The export reuses the same in-process MLT graph builder used by preview and
connects an MLT `avformat` consumer to an independent graph created from the
frozen `TimelineSnapshot`. The consumer uses the already audited dynamically
linked LGPL FFmpeg libraries. No `melt.exe`, `ffmpeg.exe`, subprocess, web
runtime, cloud service, GPL codec, or new bundled codec is introduced.

This is preferred over manually pulling every video frame and PCM block from
MLT into a second FFmpeg scheduler. A manual loop would provide lower-level
control but would duplicate frame-rate conversion, audio resampling, mux timing,
and end-of-stream behavior already implemented by MLT's LGPL avformat consumer.
It is also preferred over a platform-specific Media Foundation pipeline because
that would duplicate the MLT composition and make Windows and macOS exports
visually divergent.

MLT documents the avformat consumer as an in-process FFmpeg output service with
`vcodec`, `acodec`, profile, frame-rate, sample-rate, channel, and thread
properties. FFmpeg documents `h264_mf` as supporting both hardware and software
encoding on Windows. Those properties are still treated as untrusted runtime
capabilities: availability by name is insufficient, every selected encoder must
pass a real short encode before a user job starts.

## Presets and media contract

Two immutable presets are initially exposed:

| ID | Raster | Rate | H.264 target | AAC target |
| --- | --- | --- | --- | --- |
| `h264-1080p30` | 1920x1080 progressive | 30/1 | 12,000,000 bit/s | 192,000 bit/s |
| `h264-2160p30` | 3840x2160 progressive | 30/1 | 45,000,000 bit/s | 256,000 bit/s |

Both produce MP4 with square pixels, Rec.709 signaling, YUV 4:2:0 video, AAC-LC
48 kHz stereo audio, a keyframe interval no longer than two seconds, and fast
start metadata when the audited muxer supports it. The graph preserves the
existing transform, crop, opacity, title, caption, gain, fade, and z-order
semantics. Scaling uses the MLT profile; source aspect ratio is preserved by the
existing composition rather than stretching individual assets.

An empty timeline is rejected. Offline, missing, corrupt, redirected, or
identity-changed media is rejected with the specific asset ID before output
creation. A revision accepted by export is copied into the render job. Later
edits, undo, redo, save, scene changes, or project reopen do not mutate that job.

## Encoder selection and commercial boundary

`ExportEncoderProbe` returns ordered, evidence-bearing capabilities rather than
a Boolean list. Each capability records codec name, hardware/software mode,
pixel formats, maximum tested raster, the FFmpeg build identity, and the result
of a short black-frame plus silent-audio MP4 encode at the requested preset.

Windows selection order is:

1. available hardware H.264 wrappers from `h264_nvenc`, `h264_qsv`, and
   `h264_mf` with hardware mode forced;
2. `h264_mf` with hardware mode disabled as the required software fallback.

The first candidate that completes the real preset preflight and passes the
in-process media probe is selected. A hardware initialization or failure within
the first 60 rendered frames removes its partial file and retries the whole job
once with the preflighted software fallback. Later hardware failures terminate
the job rather than silently restarting a long render. The diagnostic always
states the attempted and final encoder.

The current dynamic LGPL FFmpeg build remains configured without
`--enable-gpl` and `--enable-nonfree`. `libx264` is not introduced. Cisco
OpenH264 is not bundled by default because Cisco's patent-paid binary conditions
require separate download, user control, and prescribed notices, while a
self-built binary does not inherit that patent arrangement. H.264 patent,
Windows codec, distribution-region, and store-policy review remains an R4 legal
and packaging gate; this document is engineering provenance, not legal advice.

On macOS the same capability interface can select `h264_videotoolbox`, but this
Windows machine cannot physically verify that path. R1-06 must not claim macOS
export success without a native build and physical encode gate.

## Architecture and responsibilities

### Domain and port layer

`RenderPreset` gains a stable preset ID and explicit fallback policy while
retaining validated typed raster, frame-rate, and bitrate values.
`RenderRequest` gains a project identity and overwrite policy. Destination paths
must be absolute `.mp4` paths with a normal filename and existing non-reparse
parent directory; traversal components and device paths are rejected.

`IRenderJob` remains the Qt-free polling/cancellation port. Its implementation is
thread-safe. `progress()` never regresses and reports rendered timeline time,
not wall-clock estimates. Terminal states are immutable. `cancel()` is
idempotent and returns only after cancellation is accepted; destruction joins
the worker and never leaves a running thread.

### Persistence

The next available migration when R1-06 resumes adds `render_jobs` with:

- render job ID, project ID, frozen timeline ID and revision;
- canonical preset ID and serialized validated preset values;
- requested destination and generated partial path;
- state, rendered-through nanoseconds, total duration, and fraction;
- attempted encoder, selected encoder, fallback reason, and causal diagnostic;
- created, started, updated, and finished UTC timestamps.

Allowed state transitions are `pending -> running -> publishing -> completed`,
`pending|running -> cancelling -> cancelled`, and
`pending|running|cancelling|publishing -> failed`. A publishing row contains the
finalized file hash and destination identity before the atomic rename. A
completed row is written only after the final file is probed and atomically
published. Project reopen converts stale pending/running/cancelling rows to
recoverable failed rows. A stale publishing row is reconciled by exact hash and
identity: a matching final file completes the row, while a matching partial is
quarantined and fails the row. Cleanup touches only the exact recorded artifact
after revalidating its identity and name.

### Render engine

`MltRenderJob` owns a dedicated worker, MLT repository lease, graph, profile,
consumer, cancellation flag, and immutable request. It never borrows the live
preview graph. The existing graph-plan/build code is shared so preview and
export cannot drift in transform or audio semantics.

The worker performs this sequence:

1. validate snapshot, assets, destination parent, output collision, and package
   identity;
2. estimate required bytes from total duration and combined target bitrate,
   add 20 percent plus 512 MiB reserve, and reject insufficient disk space;
3. persist `pending`, preflight candidates, choose an encoder, and persist
   `running`;
4. create an exclusive partial file beside the destination using
   `.<filename>.<job-id>.partial.mp4` so final rename stays on one volume;
5. build the frozen MLT graph and run the avformat consumer with frame dropping
   disabled, bounded queues, one encoding thread unless the selected encoder
   owns its own worker pool, and the exact preset properties;
6. update progress at a bounded rate of at most ten durable writes and UI
   publications per second;
7. finalize the MP4, fsync/close it, and probe H.264 video, AAC audio, raster,
   frame rate, nonzero packet counts, and duration within one video frame plus
   one AAC frame of the frozen timeline;
8. persist `publishing` with the finalized hash and destination identity,
   atomically rename the partial file, then commit `completed`; if that final
   commit fails, immediately rename back when safe or leave the publishing
   intent for exact recovery reconciliation.

The destination is never overwritten implicitly. If UI-confirmed replacement is
requested, the existing regular file identity is captured before rendering and
rechecked immediately before a replace operation. A changed target fails safely.

### Application and UI

`ExportWorker` serializes persistence, probe, and engine calls off the GUI
thread. `ExportController` publishes immutable Qt properties for preset list,
selected preset, destination, state, fraction, rendered/total time, selected
encoder, fallback notice, short status, and detailed diagnostic. It exposes
choose destination, start, cancel, retry, and reveal completed file actions.

The Export page is model-driven and keyboard accessible. Start is disabled for
an invalid destination, busy project open, offline asset, or active export.
Cancel remains visible during pending/running/cancelling. Progress never displays
100 percent before atomic publication. Closing the page does not cancel a job;
closing the application requests cancellation and drains the worker within a
bounded deadline.

## Failure and cancellation semantics

- Any preflight failure creates no user-visible output.
- Hardware early failure retries software at most once and records both causes.
- Disk-full, mux, graph, probe, or rename failure never publishes a final file.
- Cancellation before start, during graph build, encode, flush, probe, or publish
  is covered. A publish already atomically completed wins the race and reports
  completed; otherwise cancellation removes the partial and never reports 100%.
- Failure to delete a partial quarantines it with the job ID and reports the
  exact path; it is never mistaken for a successful export.
- Persistence failure before publishing stops the render before externally
  visible output. Failure after the atomic rename never reports false success:
  it rolls the file back when safe or leaves a durable publishing intent that
  exact hash-and-identity recovery reconciles on reopen.
- Controller or application destruction joins all workers and MLT consumers;
  `QThread: Destroyed while thread is still running` is a release-blocking
  failure.

## Verification gates

Development remains test-first. Required automated evidence includes:

- preset, request path, state-transition, progress-monotonicity, and cancellation
  value tests;
- migration 004 checksum, upgrade, constraint, crash-reopen, and atomicity tests;
- encoder discovery plus actual hardware success/rejection and forced software
  fallback tests with injected candidates;
- real MLT short renders for 1080p and supported 4K, with H.264/AAC stream,
  raster, rate, duration, frame-pixel, PCM, title/caption, transform, and fade
  assertions through the in-process probe;
- cancellation at startup, midpoint, finalization, and publication race points;
- missing media, full disk, destination collision/identity change, corrupt output,
  persistence failure, and retry tests proving no partial success;
- UI responsiveness, accessible actions, exact progress/action parity, and
  repeated controller destruction tests;
- complete `/W4 /permissive- /utf-8 /WX` build, sequential and parallel CTest,
  hidden app probe, shipping import/link audit, forbidden runtime scan, and
  independent review.

The R1-06 physical acceptance exports a Unicode project containing screen,
camera PIP, two audio sources, title, caption, cuts, gain, and fades to both
1080p and the supported 4K path. R1-07 then performs the separate full 30-minute
capture-edit-export and A/V synchronization closure.

## Authoritative references

- MLT avformat consumer: https://www.mltframework.org/plugins/ConsumerAvformat/
- FFmpeg codecs and Media Foundation encoders:
  https://www.ffmpeg.org/ffmpeg-codecs.html
- FFmpeg LGPL compliance guidance: https://www.ffmpeg.org/legal.html
- Microsoft Media Foundation H.264 encoder:
  https://learn.microsoft.com/en-us/windows/win32/medfound/h-264-video-encoder
- Cisco OpenH264 binary conditions:
  https://www.openh264.org/BINARY_LICENSE.txt
