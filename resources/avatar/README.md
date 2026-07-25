# Default avatar puppet

`model.inp` is the **Arch-chan** Inochi2D puppet, shipped as Creator Studio's
default rigged avatar. It is loaded at runtime by the audited Inochi2D C-FFI
runtime; no model bytes are compiled into the binary.

- **License:** CC0 1.0 Universal (public-domain dedication). Commercial use is
  permitted and no attribution is required. The full text is in `LICENSE`.
- **Origin:** RavioliMavioli's Arch Linux mascot, Inochi2D rig. Archival forks:
  <https://github.com/Speykious/arch-chan> and
  <https://github.com/orowith2os/Arch-Chan-Inochi2D>.
- **`model.inp` SHA-256:** `56a32525d1205a4f548ab27bc158379aa0d2122cf822ee28f8ec01c16205f00c`
- **Recorded in:** `legal/OSS_BOM.csv`.

## How it is used

`src/main.cpp` builds the real `Inochi2dAvatarRenderer` when both
`<appDir>/inochi2d-runtime/` (the audited runtime) and
`<appDir>/resources/avatar/model.inp` exist beside the executable; the CMake
`creator_studio` target stages both after every build. If the runtime is not
present (a build without `CS_INOCHI2D_ROOT`), the app falls back to the
first-party placeholder avatar, so the model file alone is harmless.

Face tracking drives the rig through the puppet's own one-dimensional
parameters (`Eye:: Left/Right:: Open`, `Eyebrow:: Left/Right`,
`Mouth:: Jaw Open`, `Mouth:: Pucker / Widen`, `Head Tilt`); see
`inochi2dAvatarBindings()` in `src/avatar/inochi2d/Inochi2dAvatarRenderer.cpp`.

To swap in a different puppet, replace `model.inp` (or `model.inx`) with another
Inochi2D model whose license permits redistribution, and record it in the BOM.
