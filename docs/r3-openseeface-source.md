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
