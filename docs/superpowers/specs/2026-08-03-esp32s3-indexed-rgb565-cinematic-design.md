# ESP32-S3 Indexed RGB565 Cinematic Design

## Goal

Play the published TVideo cinematic lesson on the LCDWiki ESP32-S3 at 480x320 and 10 fps without GIF conversion, resolution reduction, extra JPEG loss, decode timeouts, display DMA races, watchdog resets, or changes to the admin authoring experience.

The robot SD pack remains 27 assets: eight static lesson assets and nineteen device cinematic assets. The browser/admin preview continues to use the existing MJPEG MP4 derivative. The ESP32-S3 pack selects a new predecoded RGB565 derivative for each cinematic cue instead of downloading the MP4.

## Evidence And Decision

Physical HIL on device `14:c1:9f:d1:ac:20` measured:

- SD/FAT read of one 33-40 KB JPEG sample: 27-34 ms.
- ESP32-S3 ROM/software JPEG decode to 480x320 RGB565: 149-164 ms.
- Copy of a 307,200-byte framebuffer in PSRAM: about 10 ms.
- Synchronous LVGL refresh with software landscape rotation: 96-100 ms.
- Total current frame cost: approximately 283-307 ms.

The decode stage alone exceeds the 100 ms frame interval, so raising a timeout, moving the small decoder workspace, or pipelining the existing single decoder cannot produce sustained 10 fps. The accepted design removes JPEG decode from robot playback and removes LVGL software rotation from the cinematic frame path.

## Published Derivatives

Each cue has two immutable, content-addressed outputs from the same rendered frame sequence:

1. `preview`: the existing 480x320, 10 fps, audio-free MJPEG MP4 used by admin preview and non-S3 consumers.
2. `esp32s3Rgb565`: an indexed raw RGB565 stream used only by the LCDWiki ESP32-S3 profile.

The RGB565 output is generated directly from the final lossless PNG frame sequence produced by the TVideo renderer, before MJPEG encoding. This avoids adding another lossy generation. Each landscape frame is rotated clockwise into the panel-native 320x480 memory order during derivative generation. Rotation changes storage order only; it does not resize, crop, interpolate, or alter the displayed 480x320 composition.

Publication is fail-closed. A cue is publishable for `espTft` only when both outputs are ready and agree on cue identity, source revision, renderer build SHA-256, frame count, duration, and 10 fps timing. The robot manifest exposes only the device derivative, so the SD pack still contains exactly 27 assets.

## TBOT RGB Container V1

The device media type is `application/vnd.tbot.rgb565-indexed` and the file suffix is `.trgb`.

All integers are little-endian. The file contains:

1. A fixed 64-byte header.
2. One 16-byte index record per frame.
3. Zero padding to the next 512-byte sector boundary.
4. Contiguous frame payloads, each exactly 307,200 bytes. This size is 600 SD sectors, so every subsequent frame remains sector aligned.

Header fields:

| Offset | Size | Field | Required value |
| --- | ---: | --- | --- |
| 0 | 8 | magic | ASCII `TBOTRGB1` |
| 8 | 2 | header bytes | `64` |
| 10 | 2 | format version | `1` |
| 12 | 2 | stored width | `320` |
| 14 | 2 | stored height | `480` |
| 16 | 2 | display width | `480` |
| 18 | 2 | display height | `320` |
| 20 | 2 | fps numerator | `10` |
| 22 | 2 | fps denominator | `1` |
| 24 | 4 | frame count | positive |
| 28 | 4 | duration ms | `frameCount * 100` |
| 32 | 4 | bytes per frame | `307200` |
| 36 | 4 | index offset | `64` |
| 40 | 4 | data offset | 512-byte aligned |
| 44 | 4 | pixel format | `1` = RGB565 little-endian |
| 48 | 4 | orientation | `1` = clockwise panel-native |
| 52 | 4 | header CRC32 | CRC32 with this field zeroed |
| 56 | 8 | file bytes | exact complete file length |

Each index record contains `offset:u64`, `length:u32`, and `crc32:u32`. Version 1 requires `length=307200`, sector-aligned offsets, monotonically contiguous frames, offsets within `fileBytes`, and a per-frame CRC32. The whole file also retains the existing SHA-256 asset checksum used by preload and SD attestation.

Malformed headers, overflow, truncated index tables, invalid geometry/timing, invalid CRCs, or a mismatch between declared manifest metadata and parsed container metadata fail before display ownership changes.

## Backend And Admin Contract

The derivative repository records device-output identity separately from preview-output identity. Existing MP4 URLs and preview behavior remain backward compatible.

The published cue projection gains a profile-specific device asset with:

```json
{
  "mediaType": "application/vnd.tbot.rgb565-indexed",
  "codec": "rgb565le",
  "containerVersion": 1,
  "width": 480,
  "height": 320,
  "storedWidth": 320,
  "storedHeight": 480,
  "orientation": "panelNativeClockwise",
  "fps": 10,
  "frameCount": 95,
  "durationMs": 9500
}
```

Admin preview continues to reference the MJPEG MP4. Publish readiness displays both statuses and blocks publish if the device derivative is missing, stale, has a different frame timeline, or fails validation. Re-publishing identical source content reuses the same content-addressed outputs.

## ESP Server And SD Pack

For the LCDWiki ESP32-S3 `espTft` capability, the ESP server selects `esp32s3Rgb565` and projects it into the existing 27-record SD pack. Other profiles continue selecting MP4.

The prepare payload remains renderer v4 and template version 2. Its asset metadata identifies `.trgb`, the new media type, container version, native geometry, SHA-256, byte count, duration, frame count, and 10 fps. The strict start control remains unchanged:

```json
{"command":"start","cueId":"barn-listen","commandSequenceId":8}
```

Asset sync remains atomic: download to staging, verify byte count and SHA-256, validate the complete TRGB header/index, fsync, then rename. A bad device derivative never replaces the last verified cache entry.

## Firmware Playback

The firmware adds a bounded TRGB parser independent of the MP4 parser. It validates the full header and index at prepare time, compares parsed metadata with the prepare payload, allocates two 307,200-byte PSRAM framebuffers, verifies and reads frame zero, and only then accepts prepare.

Playback uses two buffers:

- `front`: owned by the LCD DMA transfer until the panel completion callback fires.
- `back`: filled from the next sector-aligned SD frame while `front` is transferring.

Entering cinematic mode performs these operations transactionally:

1. Acquire the display lock.
2. Stop the LVGL timer task with `lvgl_port_stop()`.
3. Wait for any existing LVGL/panel transfer to finish.
4. Mark cinematic display ownership active.
5. Release the display lock and submit the panel-native 320x480 buffer directly with `esp_lcd_panel_draw_bitmap()`.

The LCD completion callback releases a semaphore and makes the completed buffer reusable. No framebuffer may be overwritten while owned by LCD DMA. The render worker schedules frames from the monotonic lesson clock, reads into the available back buffer, verifies CRC32, and queues the buffer when its presentation time arrives. It does not call `lv_refr_now()` and does not software-rotate frames.

Leaving cinematic mode waits for the final transfer, clears cinematic ownership, resumes LVGL with `lvgl_port_resume()`, invalidates the active screen, and requests one normal refresh so status bars and lesson UI are restored. Stop, cancel, parsing failure, read failure, CRC failure, assignment/session replacement, and destructor paths all use the same idempotent cleanup.

## Timing And Failure Policy

The required steady-state acceptance target is:

- sustained 10 fps for every production cue;
- no decode work during playback;
- SD read plus CRC of a frame completes within 70 ms at p99;
- panel submission does not synchronously wait for the full transfer;
- no more than one dropped frame per 300 presented frames;
- no consecutive dropped frames;
- no parser, read, CRC, DMA ownership, watchdog, reset, or reboot error.

If the next frame is not ready, firmware retains the last complete frame and reports a timing error; it never displays a partially read buffer. Repeated lateness fails the cue rather than silently claiming smooth playback. MP4 remains supported as an explicit backward-compatible renderer path, but the LCDWiki ESP32-S3 production manifest must select TRGB.

## Resource Bounds

Runtime PSRAM use is approximately 614,400 bytes for two frames plus a small index window. The parser does not load the complete index for long cues; it reads validated records on demand and may cache a bounded window. Internal SRAM contains only synchronization primitives and small sector-aligned I/O metadata.

TRGB files are larger than MP4, approximately 3.072 MB per second of cue duration. The SD card has sufficient capacity, while content addressing, reuse, garbage collection, and atomic sync prevent duplicate active packs. Admin must show estimated robot-pack bytes before publish.

## Testing

Backend tests prove deterministic TRGB generation, panel-native rotation without scaling, exact frame count/timing, header/index/CRC correctness, dual-output readiness, admin preview retention, content-addressed reuse, and a 27-asset ESP32-S3 manifest.

ESP server tests prove capability selection, exact prepare metadata, strict control payloads, atomic sync, rejection of corrupt TRGB files, cache reuse, and MP4 fallback for non-S3 profiles.

Firmware native tests prove parser bounds and overflow handling, metadata comparison, CRC rejection, double-buffer ownership, frame clock behavior, stop/cancel cleanup, LVGL stop/resume symmetry, and failure without partial presentation. Board-level fakes verify that a buffer cannot return to the reader before the LCD completion callback.

Physical HIL uses the published farm v7 assignment and verifies all 27 assets, every cinematic cue transition, Google Live coaching, physical actions, sustained frame cadence, SD read timings, panel callback timings, heap minima, and absence of decode/DMA/watchdog/reset errors. Completion requires visual confirmation on the physical robot after automated evidence passes.

## Rollout And Rollback

The new media type and firmware parser deploy before the production manifest selects TRGB. A capability flag gates device selection to the single pilot robot. Existing MP4 assets and the current production server image remain available throughout rollout.

Rollback disables the TRGB capability projection and assigns the prior published MP4 manifest. It does not delete cached assets or require a firmware downgrade. Production assignment and nudge remain scoped to device `91deb5af-c1c0-416b-956d-266d510eac5e` during HIL.
