# R2-07 Full Product Verification

Date: 2026-07-20 (Asia/Seoul)

Environment: Windows x64, enabled FFmpeg 8.1.2, MLT 7.40.0, RNNoise
0.1.1, whisper.cpp 1.7.6, and the pinned `tiny.en` model.

## Result

R2-07 passed on the enabled product path and the retained physical 30-minute
R1 package. The workflow generated a local Whisper proposal, rejected one
proposal without changing the project, approved and persisted another,
reopened the project, cancelled an export without publishing a partial, and
retried to an atomically published MP4.

## Physical evidence

- Retained package:
  `%TEMP%\creator-studio-r2-07-물리검증\closure-run-v10-20260720-134000\30분 강의-r2.cstudio`
- Published output:
  `%TEMP%\creator-studio-r2-07-물리검증\closure-run-v10-20260720-134000\R2 최종 강의.mp4`
- Physical R2 suite: 5/5 PASS in 1,532.69 seconds.
- Transcript segments: 1; generated cut proposals: 1. The transcript was
  approved and the remaining proposal was explicitly discarded.
- Published size: 56,548,748 bytes.
- SHA-256:
  `6c4a8d204bbf36bd107b1b13bc9d97bc39cfbf27979da5de1a7e87e6a4e74b5e`.
- Probed duration: 1,799,573,333,333 ns; video H.264; audio AAC,
  48 kHz stereo.
- Immediate-cancel render job: durable `Cancelled`, with no published output
  or surviving partial. Retry job: durable `Completed`.
- Process handles: 208 initial, 489 maximum, growth 281.
- Working set: 29,265,920 bytes initial, 1,957,646,336 bytes maximum, growth
  1,928,380,416 bytes, below the 2 GiB acceptance ceiling.
- The retained R1 capture audio is completely gated silence, so physical
  measurement correctly reports no finite LUFS/true-peak value. A separate
  non-silent enabled acceptance proves the two-pass export reaches the -14 LUFS
  target with final true-peak limiting.

## Product and lifecycle verification

- The composition root creates the audited local transcription provider,
  proposal controller, project audio loader, and cursor recording binding.
- Generated transcript/cut data remains pending until explicit approval.
  Reject, cancellation, stale revision, decode/provider/store failures, and
  serialization failures preserve the original project revision and files.
- Each preview/export graph receives an independent RNNoise, compressor, and
  limiter chain. Loudness pass one streams bounded blocks; pass two uses a fresh
  cleanup chain, static gain, and final true-peak limiting.
- `Running -> Cancelling -> Cancelled` is durably persisted even when cancel
  wins the render-start race.
- MLT decoder and concat producers are reused within bounded caches. The
  audited MLT runtime includes the fail-closed PThreads4W lazy mutex-events
  overlay identified as
  `creator-studio-pthreads4w-v3.0.0-lazy-mutex-events-v1`.

## Automated verification

| Check | Result |
| --- | --- |
| Enabled build | 141/141 build steps PASS |
| Focused R2 regression | 177/177 PASS in 179.08 seconds |
| Cursor native source lifetime stress | 2,500/2,500 measured iterations PASS |
| MLT/RNNoise bootstrap policies | PASS |
| Full sequential CTest, initial run | 1,319/1,320 PASS |
| Compact layout failure from full run | fixed; targeted rerun PASS |
| Complete QML smoke rerun | 13/13 PASS |
| Final changed-area rerun | 52/52 PASS (45 unit/policy/QML + 7 MLT/export) |
| `git diff --check` | PASS |

The only failure in the initial sequential run was the 360x640 Studio capture
pane inheriting its 900-pixel scroll content as a layout minimum. Giving the
pane a zero layout minimum keeps it inside the viewport and leaves the full
device workflow reachable through scrolling. The fix is loaded directly from
the QML source, and both the exact failing test and all QML smoke tests passed
afterward; together these results cover all 1,320 registered tests on the final
tree.

## Scope boundary

This evidence closes the Windows R2 product path. The retained source audio is
silent, so it is not presented as a physical loudness-target sample. The
finite -14 LUFS assertion comes from the enabled non-silent acceptance fixture,
while the retained 30-minute package proves the real proposal, persistence,
cancellation, retry, duration, resource, and publication paths.
