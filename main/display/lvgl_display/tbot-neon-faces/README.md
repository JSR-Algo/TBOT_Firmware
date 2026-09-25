# TBot Neon Mochi Faces

21 robot emotions mapped exclusively to four user-supplied TJBot animations.
Original neon GIFs are retained on disk but are not included in the asset pack.

## Emotions (21)
neutral, happy, laughing, funny, sad, angry, crying, loving, embarrassed,
surprised, shocked, thinking, winking, cool, relaxed, delicious, kissy,
confident, sleepy, silly, confused

## Preview
Open `preview.html` in a browser to see the current faces animated.
`preview/contact_sheet.png` shows the original neon collection only.

## Specs
- TJBot replacements: 240×160, black background, 64 shared colors,
  120–145 frames/clip at approximately 24 FPS. All source frames, their timing,
  and infinite looping are preserved. A shared palette improves compression.

For the LCDWIKI standard conversation layout, the face fills the entire
480x320 panel. For these 240x160 GIFs, the decoder converts to an opaque RGB565
480x320 frame in PSRAM with integer 2x scaling. LVGL then draws at native size,
avoiding the per-pixel ARGB scaling path. Allocation failure retains the standard
GIF renderer; other GIF sizes retain their original behavior. Header/status
and subtitle bars remain above the face with transparent backgrounds and white
text/icons, including after theme changes. No strips are reserved or painted
over the face. Lesson visuals retain their layout.

`LvglGif` logs `gif_perf` every 10 seconds of continuous playback. `fps_x10`
measures delivered frame callbacks, `decode_avg_us` includes GIF decode, color
conversion and callback work, and `max_gap_ms` reports the longest callback gap.
This is software delivery timing, not a camera measurement of panel refresh.

Device check on 2026-09-07 (ESP32-S3, full-screen standby face): the original
288x192 ARGB path delivered 2.4–2.6 FPS; 240x160 with native RGB565 delivered
6.1–7.8 FPS across three 10-second windows. Source timing is 24 FPS, but the
renderer still does not sustain that rate. Background work caused occasional
1.3-second gaps; the last measured window had a 150 ms maximum gap. Build,
native RGB565 conversion test, six Python tests, flash hash verification,
asset loading and management heartbeat passed. Conversation audio quality
and physical panel cadence were not measured by this check.

## TJBot replacements (2026-09-07)

| Source in `../eyes/neweye/` | Firmware emotion |
| --- | --- |
| `TJBot-02-Laughing-PR-Talking-480x320.gif` | `laughing` |
| `TJBot-04-Sunglasses-PR-Talking-480x320.gif` | `cool` |
| `TJBot-07-Dizzy-PR-Talking-480x320.gif` | `confused` |
| `TJBot-08-Crying-PR-Talking-480x320.gif` | `crying` |

Regenerate with `python3 ../eyes/neweye/convert_neweye.py` from this directory
(requires Pillow). Missing sources are skipped during conversion. Packing
requires all four optimized GIFs and fails if one is absent.

| New GIF | Emotions |
| --- | --- |
| `cool.gif` | neutral, cool, relaxed, confident, sleepy |
| `laughing.gif` | happy, laughing, funny, loving, winking, delicious, kissy, silly |
| `confused.gif` | thinking, confused, embarrassed, surprised, shocked |
| `crying.gif` | sad, crying, angry |

The aliases share GIF bytes. No old face is packed or used as fallback.
When more GIFs arrive, add them to the converter and update `face_groups` in
`scripts/build_default_assets.py` to give the relevant emotions their own GIFs.
The preview grid reflects the packed mappings.
Rebuild the assets image after regenerating GIFs: the existing CMake command
does not track GIF changes in incremental builds. From the firmware root:

```sh
python3 scripts/build_default_assets.py --sdkconfig sdkconfig \
  --output build/generated_assets.bin --builtin_text_font tbot_vietnamese_20_4 \
  --emoji_collection tbot-neon-faces \
  --esp_sr_model_path managed_components/espressif__esp-sr/model \
  --xiaozhi_fonts_path managed_components/78__xiaozhi-fonts
```

This command matches the current LCDWIKI configuration. Verify the generated
image fits the configured assets partition before flashing.
TJBot source files were supplied by the user; the attribution below describes
the original neon collection.

## Attribution / License
Restyled from **Watcher-Mochi** (https://github.com/pham-tuan-binh/watcher-mochi),
Apache-2.0 — original Dasai Mochi animations by dasai.co. These derivatives keep
the Apache-2.0 license. For commercial product use, review the Dasai Mochi brand
terms or commission an original character.
