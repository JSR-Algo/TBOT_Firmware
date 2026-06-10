# W01 D01 Barn Lesson Render Pack

Generated: 2026-06-02 15:36:58 +0700

Source app: `TBot-Child-Companion`

Lesson: `w01-d01-barn-say-it` - `This Is a Barn`

This folder packages one complete mobile lesson scene so another renderer can
rebuild it on the robot: background, TeeBot robot poses, teaching object, script,
tap target, source metadata, and visual references.

## Start Here

1. Open `preview.html` to see the packaged scene stack.
2. Read `lesson.json` for the exact app lesson data and all prompts.
3. Read `render-contract.json` for the renderer-facing layer order, placement,
   robot pose mapping, asset paths, and checksums.
4. Use `STORYBOARD.md` as the step-by-step production handoff.

## Scene Layers

Layer 1 - background:

- File: `assets/background/barn-round-field.mp4`
- Poster: `assets/background/barn-round-field-poster.jpg`
- App source: `assets/videos/backgrounds/scenes/barn-round-field.mp4`
- Video: 1280x720 H.264, 24 fps, about 6.04 seconds, looped.
- Fit: cover/full-bleed.
- Alt caption: `A clay barn floating in a bright sky`.

Layer 2 - teaching object:

- Primary object: `barn`
- File: `assets/objects/barn.png`
- Support focus objects: `assets/objects/farm.png`, `assets/objects/hay.png`
- Render behavior from mobile: center the object in the scene, with top padding
  around 8 percent; object size is `clamp(140px, stageWidth * 0.38, 300px)`.
- Tap target: normalized center `x=0.5`, `y=0.5`, `w=0.42`, `h=0.42`, active
  from `0.5s` to `5.5s`.

Layer 3 - TeeBot robot:

- Renderer contract: `teebot-renderer.v1`
- Character: `bright-no-feet-screen-tee`
- Main pose assets: `assets/robot/poses/`
- Atlas: `assets/robot/bright-sprite-atlas.png` and
  `assets/robot/bright-sprite-atlas.json`
- Face rig sources: `assets/robot/rive-source/`
- Current app default: PNG body pose plus procedural face overlay.
- Rive note: no `.riv` runtime export is included. The SVG files are included
  for a friend to author or inspect a Rive face rig if needed.

## App Routes And URLs

Changed app routes: none. This is a handoff/export package only.

Source route in the mobile app: `/(main)/lesson-player`, using the selected
lesson from `CURATED_LESSONS`.

## Verification Notes

The assets were copied from the current local app files, not generated from
memory. `CHECKSUMS.sha256` records the packaged file hashes.

