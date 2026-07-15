# ADR-0002: Source-separated Recording

## Status
Accepted

## Decision
Record screen, camera, microphone, system audio, avatar motion, and interaction telemetry separately. Optionally record a composite preview track.

## Rationale
This enables post-recording layout changes, avatar replacement, audio mixing, cursor effects, and format adaptation.

## Consequences
- More disk usage
- More complex synchronization
- Strong differentiation and non-destructive editing
