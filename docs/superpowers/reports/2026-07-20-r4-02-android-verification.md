# R4-02 Android verification

## Decision

The Android capture/export implementation is code-complete on this workspace.
Both Android ABIs package, and the x86_64 API 35 emulator executes real
MediaCodec/MediaMuxer recording and timeline export. This report does **not**
claim physical arm64 acceptance: no physical Android device is connected.

## Automated and emulator evidence

- Windows request planning proves that one enabled video clip with embedded
  audio plus generated title and caption rasters produces three visual rows and
  one audio row. Missing text rasters and offline media fail explicitly.
- The existing `ExportControllerTest` set verifies cancellation before worker
  entry, cancellation/drain on destruction, foreground-only start, active
  export cancellation on backgrounding, scoped publish callbacks, and device
  height budgets.
- `AndroidTimelineExportPolicy` checks the platform composition root, decoder,
  compositor, audio-envelope mixer, muxer, cancellation, durable publication,
  and no-silent-omission boundaries.
- `android-x86_64-debug` and `android-arm64-debug` APK builds pass.
- Emulator: API 35, `sdk_gphone64_x86_64`, ABI `x86_64`.
- Emulator recording self-test result:
  `CreatorStudioCodec: PASS video=3322 audio=4256`. Completion now reopens each
  muxed segment and requires the expected H.264 or AAC track; non-empty bytes
  alone are not accepted.
- Emulator export self-test result:
  `CreatorStudioExport: PASS bytes=4557 video+overlay+audio+scoped+cancellation`.
  The test decodes an H.264 source, composites a transformed PNG overlay,
  decodes and mixes AAC audio, creates a combined H.264/AAC MP4, decodes a
  rendered pixel, copies through a MediaStore content URI, and proves immediate
  cancellation leaves no destination artifact.

## Device matrix

| Gate | x86_64 emulator | Physical arm64 |
|---|---|---|
| APK install and cold start | Pass | Pending — no device |
| H.264 source segment with verified video track | Pass | Pending — no device |
| AAC source segment with verified audio track | Pass | Pending — no device |
| Timeline video decode + transformed overlay | Pass | Pending — no device |
| Timeline audio decode/mix + final MP4 | Pass | Pending — no device |
| Scoped MediaStore destination publication | Pass | Pending — no device |
| Export cancellation and partial cleanup | Pass | Pending — no device |
| Foreground/background cancellation policy | Automated controller pass | Pending device lifecycle run |
| Screen projection grant/deny/revoke segment | Contract/policy only | Pending protected-surface run |
| Camera and microphone segments | Contract/policy only | Pending real sensor run |
| Session reopen/recovery | Package/recovery automation pass | Pending force-stop run |
| Thermal-pressure downgrade/block | Policy automation pass | Pending real thermal run |
| Hardware codec selection and sustained encode | Not representative | Pending hardware codec/thermal run |

The pending physical rows are release evidence gates, not missing fallback
implementations and not emulator passes. They must be run on a connected arm64
device before claiming Android physical-device acceptance.
