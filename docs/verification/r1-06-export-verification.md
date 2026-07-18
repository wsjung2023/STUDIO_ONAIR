# R1-06 Production Export Verification

## Delivered boundary

- Immutable, revision-frozen H.264 MP4 requests for 1080p30 and 2160p30.
- Physical encoder preflight in the order NVENC, QSV, Media Foundation
  hardware, and Media Foundation software fallback. A codec name alone is not
  accepted as capability evidence.
- Independent MLT export graphs with H.264 video, AAC audio, progress,
  cancellation, validated output probing, and same-volume atomic publication.
- SQLite migration 004 render-job records, encoder diagnostics, exact partial
  path, SHA-256 and file-identity publication evidence, and crash recovery.
- A dedicated Qt worker and accessible Export page. Closing the page or the
  controller cancels and joins the render job; the publication boundary cannot
  be cancelled after its durable intent is committed.

R1-07 is not covered by this report. The required real 30-minute
capture-edit-export product run remains the next gate, before any R2 work.

## Product-path media acceptance

`ProjectExportEngineIntegrationTest` uses the same composition as the shipping
application. It creates and reopens a Unicode `.cstudio` package containing:

- a screen source and cropped camera PIP;
- separate microphone and system-audio tracks with gain and fades;
- a Unicode title and caption with generated overlay artifacts;
- a persisted timeline and real SQLite render-job store.

The test exports both product presets through the audited MLT runtime. Each
result is reopened in-process and checked for H.264, AAC, exact raster,
48 kHz stereo audio, terminal database state, selected encoder, SHA-256, file
identity, and absence of orphan `.partial.mp4` files. The combined physical
acceptance passed in 33.335 seconds on this Windows machine.

Additional existing physical suites cover representative preview pixels,
mixed PCM, transform/crop/PIP, Unicode title/caption ranges, 30-minute graph
scale, and accelerated 30-minute separated recording.

## Build and complete automated gates

- MSVC build flags observed on the product and test targets:
  `/W4 /permissive- /utf-8 /WX`.
- Product target and audited MLT staging: successful.
- Complete sequential CTest: **753/753 passed**, zero failed, in
  **274.04 seconds**.
- Complete parallel CTest (`-j4`) before the combined fixture expansion:
  **753/753 passed**, zero failed, in **92.58 seconds**. The final parallel
  rerun with the expanded screen/camera/two-audio/title/caption export fixture
  also passed **753/753** in **101.16 seconds**.
- The first interrupted verification command left a second CTest process and
  caused two fixed-fixture path collisions. Both affected tests passed alone,
  and then passed in the clean full sequential and parallel runs above.
- Export controller/QML focused gate: **9/9 passed**, including 100 repeated
  controller destruction cycles.
- Qt-free core/store suite: **483/483 passed**.
- Audited MLT suite before the combined acceptance expansion: **41/41 passed**;
  the expanded product acceptance then passed independently.
- Changed-source placeholder scan: 54 files, zero TODO/FIXME/HACK or trivial
  always-passing assertions.
- `git diff --check`: clean.

## Shipping runtime and commercial OSS audit

- The staged MLT 7.40.0 manifest and every approved runtime artifact passed
  `scripts/verify_mlt_runtime.ps1`.
- Forbidden staged files (`melt.exe`, JACK, GPL, extra Qt module matches): 0.
- Normal application imports are the audited FFmpeg DLLs, Qt, MSVC, and Windows
  system libraries.
- Delay-load imports are exactly `mlt++-7.dll` and `mlt-7.dll`.
- Hidden product launch stayed alive and reported `Responding=True` at the
  probe point, then was stopped by the verification harness.
- No new third-party dependency was introduced, so `legal/OSS_BOM.csv` did not
  change. The existing dynamic LGPL MLT/FFmpeg boundary remains in force; no
  `libx264`, GPL/nonfree build, subprocess FFmpeg, or cloud runtime was added.

This is engineering provenance, not final legal, codec-patent, signing, or
store-policy advice. Those release decisions remain R4 work.

## Platform statement

- Windows x64 is physically verified for R1-06.
- macOS was not available on this machine and is not claimed by this report.
- R1-07 must use real capture devices and the shipping application. Missing
  physical capability is a blocker to resolve, not permission to substitute a
  fake capture path.
