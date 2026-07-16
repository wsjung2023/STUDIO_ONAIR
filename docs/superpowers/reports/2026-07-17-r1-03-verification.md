# R1-03 Audited MLT Preview and Playback Verification

## Delivered

- Pinned MLT 7.40.0 (`bef9d89c0c279e558d9625dac3399c2aa3d961bc`)
  dynamic LGPL build with only `core` and `avformat` modules.
- Qt-free graph compiler and manifest verifier with canonical package paths,
  checked frame ranges, exact file-set hashes, reparse rejection, and
  fail-closed per-file component/version/source/license identity.
- Native multitrack graph with black background, core video composites, core
  audio mixes, real avformat media, deterministic full rebuild, and last-good
  graph retention after failed updates.
- Bounded owned BGRA preview frames, explicit Rec.709 YUV422 conversion,
  dimension/overflow guards, asynchronous one-frame-in-flight playback,
  generation/revision stale-frame rejection, and a QML preview surface.
- Runtime-only application staging. Windows delay-loads `mlt++-7.dll` and
  `mlt-7.dll`, registers the staged DLL directory before native use, and does
  not compile the build-machine MLT prefix into the executable.
- Empty timelines produce a deterministic one-frame black preview. Playback
  stops normally at the last timeline frame, returned native frame positions
  must exactly match their request, and the editor toolbar exposes bounded
  timeline seeking.

## Review corrections

Independent review identified eight concrete weaknesses. The final code:

- rejects a different factory root before changing global MLT environment;
- bounds native-returned dimensions and checked buffer multiplication;
- converts the explicitly configured Rec.709 limited-range compositor output;
- treats an unsized alpha pointer as unproven and safely opaque;
- checks every native transition planting result;
- proves both lower and upper video tracks at different timeline positions;
- observes successful native video/audio transition counts; and
- names the repeated-destruction test only for what it proves.

The stronger layer test exposed an additional still-image seek defect. Image
assets now repeat their decoded first frame in the MLT playlist instead of
requesting nonexistent temporal frames. A direct 600 ms random seek proves the
fix.

A second independent review found seven additional gaps. The corrected gate:

- treats empty timelines and normal end-of-timeline playback as valid states;
- rejects a native frame returned for any position other than the request;
- provides a QML seek slider tied to the timeline duration and playhead;
- validates every shipped DLL's immutable provenance and license identity;
- preloads verified delay-import libraries and retains their DLL search
  directory, with an invalid-PE test proving failure becomes `AppError`;
- extracts actual mixed PCM and proves the sum of two real WAV tracks, exposing
  and fixing missing `audioconvert` normalization plus transition ordering; and
- gives the process-global MLT factory an explicit teardown owner, with the
  Windows cross-CRT wrapper exception documented in ADR-0003.

## Physical acceptance

- A real Unicode media package with red lower and delayed blue upper tracks is
  opened through `EditorController` and the staged MLT runtime.
- Initial preview is red; seek to 600 ms is blue; play advances; pause stops.
- A copied physical runtime rejects a changed `mltcore.dll`, accepts it after
  restoration, rejects an added `melt.exe`, and accepts the clean copy again.
- Staged manifest: 156 files (13 runtime libraries, 2 runtime modules, 140 data
  files, 1 build-evidence file), with zero development files.
- Runtime modules are exactly `mltcore.dll` and `mltavformat.dll`; no
  `melt.exe`, GPL module, or top-level duplicate MLT DLL is shipped. The core
  module's LGPL `producer_melt` metadata remains upstream data, but product
  graph compilation never selects that service.

## Final gate

- Configuration: MSVC Debug, `/W4 /permissive- /WX`, MLT enabled.
- Clean build: **254/254 steps passed**, zero warnings.
- Complete sequential CTest: **478/478 passed** in 63.33 seconds.
- Parallel CTest first produced two transient failures in pre-existing SQLite
  edit-history tests; both passed immediately in isolation and the complete
  sequential gate passed. No failure was skipped or disabled.
- Native MLT suite: **16/16 passed**.
- Application suite: **91/91 passed**.
- R1 physical MLT acceptance: **2/2 passed**.
- The physical acceptance executable also delay-loads both MLT DLLs and passed
  with no MLT directory on `PATH`; the adapter loaded only its staged runtime.
- `dumpbin`: normal imports are Qt/MSVC/Windows only; MLT appears only in the
  delay-import table.
- Link command contains product libraries and no `cs_fakes`.
- A `cmake --install` rehearsal produced only the application `bin` tree
  (test-framework install rules disabled) and its installed MLT runtime passed
  the standalone verifier: 156 files, four exact dependency identities, and
  zero development artifacts.
- `creator_studio.exe` launched from the clean output with no top-level MLT
  DLLs and reported `Responding=True`.
- `git diff --check` passed.

## Remaining release scope

- This is the verified Windows R1-03 gate. macOS physical MLT staging and
  execution remain unverified on the current machine.
- Qt deployment, code signing, installer layout, codec patent/royalty review,
  and long-duration release soak tests remain explicit R4 gates.
- Final render/export remains R1-05; `MltEditEngine::render` still returns an
  explicit unsupported error rather than a false success.
