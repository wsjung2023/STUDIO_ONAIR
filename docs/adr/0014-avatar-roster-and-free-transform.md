# ADR-0014: Avatar Character Roster (11) and Free Position/Size Overlay

## Status
Accepted (first-party 2D renderer; synthetic-tracking/placeholder paths unchanged)

## Context
The built-in `CharacterAvatarRenderer` (Qt-free, CPU, supersampled software
canvas) shipped with three characters (사람/고양이/여우) and only fixed placement:
정면(centred) or one of four fixed corners. A Korean content creator asked for
(1) many more varied, appealing selectable characters, and (2) — most important —
an avatar usable as an overlay whose **position and size are freely adjustable**
("가장 중요한건 쓸만한 아바타로써 위치 크기가 자유롭게 조정 되야 하는게 중요해").

## Decision

### Roster: 3 → 11 characters
Added eight new `AvatarCharacterId`s, each with its own `drawXxx()` rig
(reusing `drawEye`/`drawBlush`/`drawBrowDots` and the `Canvas` primitives),
a catalog entry (machine key + Korean label), a backdrop gradient, and a
`drawCharacter()` dispatch case:

| key | label | notes |
|-----|-------|-------|
| `man_mid` | 아저씨 | side-parted hair, stubble/jaw shadow, heavier brows |
| `woman` | 여자 | long flowing hair, soft jaw, long lashes, lips |
| `boy` | 남자애 | small round face, spiky boyish hair, big eyes, grin |
| `girl` | 여자애 | pigtails + ribbons, big sparkly eyes |
| `teen` | 소녀(중학생) | neat bob, sailor collar, gentle |
| `dog` | 강아지 | floppy ears, cream muzzle, lolling tongue on mouth-open |
| `bear` | 곰 | round ears, muzzle, round brown face |
| `alien` | 외계인 | tapered teal head, big glossy black almond eyes (blink+catchlight), antennae |

All eleven are driven by the same nine tracking channels: blink closes the
eyelids, pupils/gaze drift with yaw+pitch, mouth opens with `mouthOpen` /
widens with `mouthWide`, brows/brow-marks raise with `browUp` (the alien's
antennae lift instead), and the whole head translates + parallaxes + rolls
with yaw/pitch/roll. `drawEye` gained an optional `sclera` colour so the alien
can reuse it for glossy black eyes while keeping a real blink lid and catchlight.

### Free transform (position + size)
`CharacterAvatarRenderer` now holds three atomics — `userScale`
(clamped 0.35x–2.5x of the base head radius `min(w,h)*0.30`) and a normalised
head centre `posX,posY ∈ [0,1]`. `render()` computes `pose.R = baseR*userScale`
and `pose.cx/cy = pos*frame` plus a small yaw/pitch parallax, then **clamps the
centre so at least part of the head always stays on-frame** (the avatar can be
moved/resized freely but never fully lost). Placement mode now selects only the
**backdrop style**: `Front` paints the opaque gradient; `Corner` leaves the
frame transparent (composites over the screen recording) and draws a legibility
card behind the avatar. 정면/코너 remain convenience **presets**
(`applyFrontPreset` / `applyCornerPreset`) that just set `posX/posY/userScale`;
any later drag or size change overrides them.

`AvatarSceneController` exposes thread-safe `avatarScale`/`avatarPosX`/
`avatarPosY` properties (with `avatarMinScale`/`avatarMaxScale`), invokable
`setAvatarScale(real)` and `setAvatarPosition(real,real)`, and a
`transformChanged` signal so the QML size slider stays in sync when a preset or
drag changes the transform. `StudioPage.qml` adds an 11-character picker, a 크기
size slider, and a drag `MouseArea` over the avatar preview that maps the pointer
to normalised `posX/posY` live. The live avatar frame is now 16:9 (640×360) to
match the Studio stage aspect, so the drag mapping matches the baked frame
instead of being pillar-boxed/stretched.

## Boundaries / non-goals
- The avatar layer (`cs_avatar`) stays Qt-/FFmpeg-/MLT-free (`cs_assert_qt_free`).
- The real webcam / OpenSeeFace UDP tracking path in `main.cpp` is **unchanged**;
  the default live avatar remains synthetic tracking + the built-in character
  renderer, honestly surfaced via `trackingLabel`.

## Tests / proof
- `CharacterAvatarRendererTest`: catalog is 11 with unique ids+keys; every
  character renders a correctly sized frame with alpha in both opaque and overlay
  modes; `userScale` grows the drawn coverage; `posX` shifts the content centroid;
  extreme positions clamp yet still draw; presets set transform + style.
- `AvatarCharacterShots` emits `{key}_{neutral,blink,mouth,turn}.png` for all 11,
  plus `transform_small_corner.png`, `transform_large_center.png`,
  `transform_moved_topleft.png`, and `overlay_over_screen.png` (a small
  repositioned avatar composited over a synthetic screen).
