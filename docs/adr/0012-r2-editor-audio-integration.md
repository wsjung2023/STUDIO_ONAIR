# ADR-0012: R2 editor and audio integration boundary

## Status

Accepted for the R2 integration pass.

## Decisions

- `CursorVisualEffectsPlan` is an immutable, optional final-frame port. MLT
  preview and export graph instances receive the same plan; the port applies
  an active zoom viewport and deterministic click emphasis after the composed
  BGRA frame is decoded. Cursor removal remains separate because it needs a
  clean plate or replacement asset.
- `MltEditEngineConfig::audioProcessingChain` is an optional
  `IAudioProcessor` chain. When present it is attached to the MLT tractor as a
  final audio filter, so preview mixed-audio requests and avformat exports use
  the same processing boundary. Failures are surfaced through the existing
  `Result`/filter-stage diagnostics.
- Loudness normalization remains an offline two-pass operation through
  `LoudnessNormalizer`; it is not silently approximated per audio block. A
  render job must provide a whole-program audio pass before enabling that
  policy. This keeps A/V timing and EBU-R128 semantics correct.
- Transcript JSON is loaded through `TranscriptStore`, displayed as a
  read-only segment list, and can be inserted into the selected caption clip.
  Existing caption edits and marked-range lift/ripple-delete remain
  non-destructive command operations.

## Verification boundary

The default Windows debug profile has MLT/FFmpeg disabled, so the local gate
verifies the pure effects, transcript, DSP, atomic publication, and policy
tests. The final physical R2 gate still requires an audited MLT+FFmpeg runtime,
camera/microphone permissions, a real recording, export cancellation, retry,
and MP4 probe on the target machine.
