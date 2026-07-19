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
