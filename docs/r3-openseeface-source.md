# R3 OpenSeeFace source boundary

The R3 avatar core now has two provider shapes:

- `ITrackingProvider` is the in-process, frame-consuming port used by an engine
  such as MediaPipe.
- `ITrackingSource` is the self-driven port for asynchronous engines. The
  `OpenSeeFaceUdpSource` implementation binds a non-blocking UDP socket,
  receives one bounded datagram per poll, and delegates byte decoding to the
  verified pure `OpenSeeFaceParser`.

The source never reads a clock. The caller supplies the project timestamp to
`poll()`, preserving the recorder's monotonic A/V timebase. Empty polls return
an empty batch; malformed datagrams return `InvalidArgument`; socket failures
return `IoFailure`. At most 32 face records are accepted per datagram, so the
source cannot grow an unbounded receive allocation.

`AvatarMotionPipeline` then applies primary-face selection, confidence gating,
calibration, One-Euro smoothing, and durable `avatar.motion` NDJSON output in
one deterministic boundary. `AvatarTrackingSession` now owns the source
lifecycle and connects `start → poll → pipeline → stop` without introducing a
hidden thread or clock. Camera pixel readback, MediaPipe/OpenSeeFace model
process supervision, Inochi2D rendering, and Studio/Editor wiring remain the
next integration stage; no model weights are bundled by this change.

`OpenSeeFaceProcessSupervisor` now provides the process-lifecycle boundary for
an optional external worker. The caller supplies the executable and complete
argument list because OpenSeeFace distributions expose different launch
switches. The supervisor owns the child handle, reaps exit codes without a
blocking poll, and terminates an owned worker during `stop()`. It does not
assume a camera, UDP port, timestamp, or model path; pair it with the UDP
source and bind the selected port in the application layer.

The application layer now also exposes `AvatarFrameTrackingCoordinator`. It
consumes the existing bounded camera preview mailbox and routes one latest
`VideoFrame` through an in-process `ITrackingProvider` and the same motion
pipeline. A capture terminal error wins over any pending frame, and an empty
mailbox produces no synthetic sample.

`AvatarParameterMapper` is the renderer-neutral model mapping boundary. A
model adapter supplies its parameter names plus scale/offset/range rules; the
mapper validates uniqueness and finite ranges, then emits deterministic,
clamped values from the canonical nine tracking channels. This makes the
Inochi2D adapter a replaceable consumer instead of leaking model-library types
into `cs_avatar`.

`AvatarRenderPipeline` completes the next handoff: it maps one timestamped
motion sample and submits the values to `IAvatarRenderer`, which returns an
immutable BGRA8 `AvatarRenderFrame`. The frame type has an explicit transparent
constructor and validates stride/byte ownership, so an eventual Inochi2D
adapter can be added without changing tracking or app capture contracts.

`AvatarModelDescriptor` adds the package-side registration contract: a versioned
JSON sidecar resolves a model path relative to itself, validates canvas bounds,
and reuses the parameter mapper for names and ranges. It deliberately does not
parse Inochi2D internals; the renderer adapter receives a verified model path
and descriptor-owned mapping instead.

Studio scene sources now include the persisted `avatar` role. It is treated as
a visual source (so transform, PIP, z-order, and enable/disable operations use
the same scene rules as camera/screen) without changing the R1 default scene
or pretending that a real model is bundled.

The recorder role map now carries `Avatar` as a first-class video track and
stores its segments below `media/avatar/<source-id>`. The live capture binding
does not claim an avatar device exists; an eventual render source must attach a
real `IVideoFrameSink` before the role can be selected for live recording.

`AvatarRenderFrame::toVideoFrame()` bridges tightly packed BGRA output into the
existing timestamped media frame contract while retaining immutable pixel
ownership. Preview and recording can therefore consume an avatar render frame
without a second copy or a renderer-specific handle.

`AvatarMotionPlayback` provides the editor-side telemetry reader. It validates
the recorded NDJSON sequence and samples canonical parameters by project time,
linearly interpolating between adjacent samples while clamping before/after
the recorded range. The result can feed the same `AvatarRenderPipeline` used
by live tracking.

`AvatarMotionPlaybackRenderer` closes the editor-time handoff: a requested
project timestamp is sampled from telemetry, rendered through the same mapper
and renderer pipeline, and returned as a normal `media::VideoFrame`.

`inochi2d::Inochi2dModelRuntime` is the optional SDK boundary. It dynamically
resolves the official C FFI, loads an `.inx` model, applies mapped one-dimensional
parameters, and advances physics. The runtime declarations mirror the SDK's
generated C ABI (including its Windows `cdecl` convention); the external SDK
library remains a deploy-time artifact. Missing libraries, missing symbols, and
model-load failures become explicit `AppError` values rather than crashing the
core build or silently rendering a fake model. GPU/draw-list rasterization is
still a separate renderer-backend step.

`AvatarSoftwareRasterizer` is that backend's CPU fallback boundary. It accepts
copied BGRA texture bytes plus validated textured triangles and emits the same
immutable `AvatarRenderFrame` used by preview and recording. It rejects malformed
indices, non-finite vertices, storage mismatches, and dimension overflow before
allocating a frame; GPU acceleration can replace this implementation without
changing the model descriptor or capture contracts.

`AvatarSoftwareRenderer` implements `IAvatarRenderer` on top of that fallback.
Its mesh-provider callback receives the mapped model parameters, returns one
validated mesh/texture snapshot, and the adapter emits an ordinary immutable
avatar frame. This closes the common live/playback render pipeline without
coupling it to a particular model SDK.

The rasterizer also composites ordered textured batches with straight-alpha
source-over blending, which is the shape needed when an Inochi2D draw list
references more than one texture.

When the optional SDK is present, `Inochi2dModelRuntime::renderSnapshot()` now
advances physics, invokes `in_puppet_draw`, validates the default 2D vertex and
32-bit index buffers, converts RGB/RGBA texture bytes to BGRA, and returns
renderer-neutral batches. Unsupported SDK symbols, layouts, malformed commands,
or texture failures remain explicit errors; the core build still has no SDK
link-time dependency.
