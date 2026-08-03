# ESP32-S3 Indexed RGB565 Cinematic Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish panel-native indexed RGB565 cinematic assets and play them at sustained 480x320/10 fps on the LCDWiki ESP32-S3 while preserving MJPEG admin preview and the 27-asset robot pack.

**Architecture:** The backend render worker creates an MJPEG MP4 preview and a `.trgb` device derivative from the same lossless PNG frames. The ESP server selects `.trgb` for the LCDWiki S3 capability and projects exact local SD metadata. Firmware validates the bounded TRGB container, double-buffers sector-aligned frames, suspends LVGL during cinematic ownership, and submits panel-native 320x480 frames directly to the LCD DMA path.

**Tech Stack:** NestJS/TypeScript/Vitest/PostgreSQL/FFmpeg; Python/pytest ESP lesson runtime; ESP-IDF C++17/LVGL/SDMMC/esp_lcd/native ASan+UBSan tests.

---

## Worktree Map

- Backend: `/Users/manhhodinh/Documents/TBOT/.worktrees/backend-google-live-tvideo-journey`
- ESP server: `/Users/manhhodinh/Documents/TBOT/robot/esp32-server/.worktrees/google-live-tvideo-journey/main/tbot-server`
- Firmware: `/Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware/.worktrees/lesson-non-hardware-completion`

Workers are not alone in these worktrees. They must preserve unrelated dirty changes, own only the files assigned below, and never revert another worker's edits.

### Task 1: Backend TRGB V1 writer and validator

**Ownership:** Backend binary container only.

**Files:**
- Create: `src/lessons/derivatives/tbot-rgb565.ts`
- Create: `src/lessons/derivatives/tbot-rgb565.spec.ts`

- [ ] **Step 1: Write the failing round-trip and corruption tests**

```ts
it('writes sector-aligned panel-native RGB565 frames with CRC32', async () => {
  const frames = [Buffer.alloc(307_200, 0x12), Buffer.alloc(307_200, 0x34)];
  const bytes = buildTbotRgb565({ frames, displayWidth: 480, displayHeight: 320, fps: 10 });
  const parsed = parseTbotRgb565(bytes);
  expect(parsed.metadata).toMatchObject({ storedWidth: 320, storedHeight: 480, frameCount: 2, durationMs: 200 });
  expect(parsed.records.map((r) => r.offset % 512)).toEqual([0, 0]);
  expect(readTbotRgb565Frame(bytes, parsed, 1)).toEqual(frames[1]);
});

it('rejects a frame whose CRC32 does not match', () => {
  const bytes = buildTbotRgb565({ frames: [Buffer.alloc(307_200)], displayWidth: 480, displayHeight: 320, fps: 10 });
  bytes[bytes.length - 1] ^= 0xff;
  expect(() => parseTbotRgb565(bytes, { verifyFrames: true })).toThrow(/frame CRC32/i);
});
```

- [ ] **Step 2: Run the test and verify RED**

Run: `npx vitest run src/lessons/derivatives/tbot-rgb565.spec.ts`

Expected: FAIL because `tbot-rgb565.ts` does not exist.

- [ ] **Step 3: Implement the exact 64-byte header, 16-byte records, alignment, CRC32, and bounds checks**

```ts
export const TBOT_RGB565_MEDIA_TYPE = 'application/vnd.tbot.rgb565-indexed';
export const TBOT_RGB565_FRAME_BYTES = 320 * 480 * 2;

export type TbotRgb565Metadata = Readonly<{
  storedWidth: 320; storedHeight: 480; displayWidth: 480; displayHeight: 320;
  fps: 10; frameCount: number; durationMs: number; dataOffset: number; fileBytes: number;
}>;

export function align512(value: number): number {
  if (!Number.isSafeInteger(value) || value < 0) throw new Error('invalid TRGB offset');
  return (value + 511) & ~511;
}
```

Use `Buffer.read/writeUInt{16,32}LE`, `read/writeBigUInt64LE`, a table-driven CRC32, checked arithmetic before every allocation, and exact magic `TBOTRGB1`.

- [ ] **Step 4: Run focused tests and verify GREEN**

Run: `npx vitest run src/lessons/derivatives/tbot-rgb565.spec.ts`

Expected: PASS.

- [ ] **Step 5: Commit the backend container unit**

```bash
git add src/lessons/derivatives/tbot-rgb565.ts src/lessons/derivatives/tbot-rgb565.spec.ts
git commit -m "feat(lessons): add indexed RGB565 container"
```

### Task 2: Backend dual-output rendering and persistence

**Ownership:** Backend media worker, repository, migration, readiness status.

**Files:**
- Modify: `src/lessons/derivatives/flattened-cinematic-media.ts`
- Modify: `src/lessons/derivatives/flattened-cinematic-media.spec.ts`
- Modify: `src/lessons/derivatives/flattened-cinematic.repository.ts`
- Modify: `src/lessons/derivatives/flattened-cinematic.repository.spec.ts`
- Modify: `src/lessons/derivatives/flattened-cinematic-worker.service.ts`
- Modify: `src/lessons/derivatives/flattened-cinematic-worker.service.spec.ts`
- Create: `src/database/migrations/118_tvideo_rgb565_derivatives.sql`
- Create: `src/database/migrations/118_tvideo_rgb565_derivatives.down.sql`
- Create: `tests/tvideo-rgb565-derivatives.migration.spec.ts`

- [ ] **Step 1: Write failing media tests for dual output and panel-native rotation**

```ts
expect(output.preview.metadata.codec).toBe('mjpeg');
expect(output.device.metadata).toEqual({
  codec: 'rgb565le', containerVersion: 1, width: 480, height: 320,
  storedWidth: 320, storedHeight: 480, orientation: 'panelNativeClockwise',
  fps: 10, durationMs: 900, frameCount: 9, hasAudio: false,
});
expect(output.device.path).toMatch(/\.trgb$/);
```

Add a 2x3 color-grid fixture and assert the stored first frame equals a clockwise rotation, with no scaling or interpolation.

- [ ] **Step 2: Run the focused tests and verify RED**

Run: `npx vitest run src/lessons/derivatives/flattened-cinematic-media.spec.ts src/lessons/derivatives/flattened-cinematic-worker.service.spec.ts`

Expected: FAIL because the service returns one MP4 output.

- [ ] **Step 3: Generate TRGB from the lossless frame directory before cleanup**

Add an FFmpeg raw extraction command equivalent to:

```ts
return ['-framerate', '10', '-i', join(frameDir, 'frame-%06d.png'),
  '-vf', 'transpose=1', '-pix_fmt', 'rgb565le', '-f', 'rawvideo', rawPath];
```

Stream 307,200-byte frames from the temporary raw output into `buildTbotRgb565`, write a temporary `.trgb`, validate it, fsync it, then atomically rename it beside the MP4. Return:

```ts
type FlattenedCinematicOutputs = Readonly<{
  preview: FlattenedCinematicOutput;
  device: FlattenedCinematicRgb565Output;
}>;
```

- [ ] **Step 4: Add additive persistence columns and fail-closed constraints**

Migration `118` adds `device_output_path`, `device_output_url`, `device_output_sha256`, `device_output_bytes`, and `device_output_metadata` to `flattened_cinematic_derivatives`. A check constraint requires all five device fields to be null or all five populated. The down migration removes the constraint and columns.

- [ ] **Step 5: Write failing repository tests, then persist both outputs atomically**

```ts
expect(query.text).toMatch(/device_output_path=\$19/);
expect(row.output?.device.metadata.codec).toBe('rgb565le');
expect(() => parseOutput({ ...valid, device: undefined })).toThrow(/invalid flattened/i);
```

Update ready commits and row parsing so TVideo v2 requires both outputs, while legacy v1 rows remain MP4-only.

- [ ] **Step 6: Run migration, media, repository, and worker tests**

Run:

```bash
npx vitest run tests/tvideo-rgb565-derivatives.migration.spec.ts \
  src/lessons/derivatives/flattened-cinematic-media.spec.ts \
  src/lessons/derivatives/flattened-cinematic.repository.spec.ts \
  src/lessons/derivatives/flattened-cinematic-worker.service.spec.ts
npm run build
```

Expected: all PASS and Nest build exits 0.

- [ ] **Step 7: Commit dual-output rendering**

```bash
git add src/lessons/derivatives src/database/migrations/118_tvideo_rgb565_derivatives* tests/tvideo-rgb565-derivatives.migration.spec.ts
git commit -m "feat(lessons): render RGB565 device derivatives"
```

### Task 3: Backend manifest, admin readiness, and 27-asset projection

**Ownership:** Backend published contract and admin status only.

**Files:**
- Modify: `src/lessons/authoring/lesson-authoring.flattened-derivatives.ts`
- Modify: `src/lessons/authoring/lesson-authoring.flattened-derivatives.spec.ts`
- Modify: `src/lessons/lesson-manifest.logic.ts`
- Modify: `src/lessons/lesson-manifest.logic.validation.spec.ts`
- Modify: `src/lessons/lesson-sd-pack.contract.ts`
- Modify: `src/lessons/lesson-sd-pack.contract.spec.ts`
- Modify: `src/lessons/authoring/lesson-authoring.controller.ts`
- Modify: `src/lessons/authoring/lesson-authoring.tvideo-journey-api.spec.ts`

- [ ] **Step 1: Write failing published-contract tests**

```ts
expect(cue.asset.mediaType).toBe('application/vnd.tbot.rgb565-indexed');
expect(cue.asset.path).toMatch(/\.trgb$/);
expect(cue.asset.metadata).toMatchObject({
  codec: 'rgb565le', containerVersion: 1, storedWidth: 320,
  storedHeight: 480, orientation: 'panelNativeClockwise', fps: 10,
});
expect(manifest.assetPack.assets).toHaveLength(27);
```

Also assert the admin status includes both `preview` and `device`, and preview URLs still end in `.mp4`.

- [ ] **Step 2: Run focused tests and verify RED**

Run: `npx vitest run src/lessons/authoring/lesson-authoring.flattened-derivatives.spec.ts src/lessons/lesson-manifest.logic.validation.spec.ts src/lessons/lesson-sd-pack.contract.spec.ts`

- [ ] **Step 3: Project device output into `espTft` manifests and retain preview output in admin DTOs**

The published cinematic cue uses the device fields as its `asset`. Admin derivative status returns:

```ts
{ cueId, state, preview: output.preview, device: output.device }
```

Publish readiness rejects missing/stale device output, timeline disagreement, wrong media type, wrong geometry, or non-current source revision.

- [ ] **Step 4: Run tests and build**

Run: `npx vitest run src/lessons/authoring/lesson-authoring.flattened-derivatives.spec.ts src/lessons/lesson-manifest.logic.validation.spec.ts src/lessons/lesson-sd-pack.contract.spec.ts src/lessons/authoring/lesson-authoring.tvideo-journey-api.spec.ts && npm run build`

Expected: PASS; robot asset count remains 27.

- [ ] **Step 5: Commit manifest projection**

```bash
git add src/lessons/authoring src/lessons/lesson-manifest.logic.ts src/lessons/lesson-manifest.logic.validation.spec.ts src/lessons/lesson-sd-pack.contract*
git commit -m "feat(lessons): publish RGB565 assets for ESP32-S3"
```

### Task 4: ESP server TRGB capability, validation, and prepare projection

**Ownership:** ESP server contract and runtime projection only.

**Files:**
- Modify: `core/lesson/flattened_cinematic_contract.py`
- Modify: `core/lesson/runtime.py`
- Modify: `core/lesson/asset_cache.py`
- Modify: `tests/test_flattened_cinematic_contract.py`
- Modify: `tests/test_lesson_asset_cache.py`
- Modify: `tests/test_lesson_conversation_integration.py`
- Modify: `tests/test_tvideo_farm_cross_repo_fixture.py`
- Modify: `scripts/project_tvideo_farm_firmware_fixture.py`

- [ ] **Step 1: Write failing exact-contract tests**

```py
assert phase["asset"]["mediaType"] == "application/vnd.tbot.rgb565-indexed"
assert phase["asset"]["containerVersion"] == 1
assert phase["asset"]["storedWidth"] == 320
assert phase["asset"]["storedHeight"] == 480
assert phase["asset"]["orientation"] == "panelNativeClockwise"
assert len(pack["assets"]) == 27
```

Add negative cases for `.mp4` paired with the RGB media type, invalid container version, wrong stored geometry, wrong frame bytes, and prepare-only metadata leaked into start.

- [ ] **Step 2: Run tests and verify RED**

Run: `pytest -q tests/test_flattened_cinematic_contract.py tests/test_tvideo_farm_cross_repo_fixture.py tests/test_lesson_conversation_integration.py`

- [ ] **Step 3: Update exact manifest and SD-pack validators**

Require device cue paths `lessons/derivatives/<sha>/<cue>.trgb`, media type `application/vnd.tbot.rgb565-indexed`, and metadata keys:

```py
{
  "codec", "containerVersion", "width", "height", "storedWidth", "storedHeight",
  "orientation", "fps", "durationMs", "frameCount", "frameBytes", "hasAudio",
}
```

Require `codec=rgb565le`, `containerVersion=1`, `frameBytes=307200`, and unchanged strict start body.

- [ ] **Step 4: Preserve compatibility metadata through cache and prepare projection**

Extend `_asset_pack_record()` and `project_flattened_cinematic_phase()` so prepare contains the new asset fields without adding them to control commands.

- [ ] **Step 5: Run the full focused ESP suite**

Run: `pytest -q tests/test_flattened_cinematic_contract.py tests/test_lesson_asset_cache.py tests/test_lesson_conversation_integration.py tests/test_tvideo_farm_cross_repo_fixture.py`

Expected: PASS.

- [ ] **Step 6: Commit ESP contract support**

```bash
git add core/lesson/flattened_cinematic_contract.py core/lesson/runtime.py core/lesson/asset_cache.py tests scripts/project_tvideo_farm_firmware_fixture.py
git commit -m "feat(lesson): project indexed RGB565 cinematics"
```

### Task 5: Firmware bounded TRGB parser

**Ownership:** Firmware container parser only.

**Files:**
- Create: `main/lesson_rgb565_stream.h`
- Create: `main/lesson_rgb565_stream.cc`
- Create: `tests/native/lesson_rgb565_stream_test.cc`
- Create: `scripts/run_host_native_lesson_rgb565_stream_test.sh`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Write failing parser tests**

```cpp
Require(file.Open(path) == LessonRgb565Status::kOk, "valid TRGB opens");
Require(file.metadata().stored_width == 320 && file.metadata().stored_height == 480,
        "panel-native geometry is parsed");
Require(file.ReadFrame(1, buffer.data(), buffer.size()) == LessonRgb565Status::kOk,
        "indexed frame reads and verifies CRC");
```

Add fixtures for bad magic/header CRC, arithmetic overflow, non-aligned offsets, truncated index, wrong frame size, frame CRC mismatch, session lease loss, and path traversal.

- [ ] **Step 2: Run the new script and verify RED**

Run: `./scripts/run_host_native_lesson_rgb565_stream_test.sh`

Expected: compile failure because parser files do not exist.

- [ ] **Step 3: Implement the parser using fixed-width checked reads**

```cpp
struct LessonRgb565Metadata {
    std::uint16_t stored_width;
    std::uint16_t stored_height;
    std::uint16_t display_width;
    std::uint16_t display_height;
    std::uint16_t fps;
    std::uint32_t frame_count;
    std::uint32_t duration_ms;
    std::uint32_t frame_bytes;
};
```

Use `LessonAssetStorageCoordinator` and `SdFatSessionGuard`, never load the complete index, validate one 16-byte record before each frame read, and calculate CRC32 while reading into the destination buffer.

- [ ] **Step 4: Run parser and existing MP4 tests**

Run: `./scripts/run_host_native_lesson_rgb565_stream_test.sh && ./scripts/run_host_native_lesson_mjpeg_mp4_test.sh`

Expected: PASS.

- [ ] **Step 5: Commit parser**

```bash
git add main/lesson_rgb565_stream.* tests/native/lesson_rgb565_stream_test.cc scripts/run_host_native_lesson_rgb565_stream_test.sh main/CMakeLists.txt
git commit -m "feat(firmware): parse indexed RGB565 streams"
```

### Task 6: Firmware cinematic LCD ownership and asynchronous direct present

**Ownership:** Firmware display transport only.

**Files:**
- Modify: `main/display/lcd_display.h`
- Modify: `main/display/lcd_display.cc`
- Modify: `main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc`
- Create: `tests/native/lesson_cinematic_display_transport_test.cc`
- Create: `scripts/run_host_native_lesson_cinematic_display_transport_test.sh`

- [ ] **Step 1: Write a failing ownership-state test around a fake panel transport**

```cpp
Require(display.BeginLessonCinematic(), "cinematic ownership starts");
Require(display.QueueLessonCinematicFrame(front.data(), 320, 480), "first DMA starts");
Require(!display.QueueLessonCinematicFrame(front.data(), 320, 480), "owned buffer cannot be reused");
transport.Complete();
Require(display.WaitLessonCinematicFrame(20), "completion releases buffer");
Require(display.EndLessonCinematic(), "LVGL resumes exactly once");
```

Assert failure paths resume LVGL, duplicate end is idempotent, and no direct draw is accepted outside cinematic ownership.

- [ ] **Step 2: Run the test and verify RED**

Run: `./scripts/run_host_native_lesson_cinematic_display_transport_test.sh`

- [ ] **Step 3: Add explicit display APIs and panel completion signaling**

```cpp
bool BeginLessonCinematic();
bool QueueLessonCinematicFrame(const std::uint16_t* pixels,
                               std::uint16_t stored_width, std::uint16_t stored_height);
bool WaitLessonCinematicFrame(std::uint32_t timeout_ms);
bool EndLessonCinematic();
```

On LCDWiki: stop LVGL under its lock, wait for prior panel completion, submit native `320x480` via `esp_lcd_panel_draw_bitmap`, signal a binary semaphore from `on_color_trans_done`, then resume/invalidate LVGL on exit. Preserve the existing LVGL path for all non-cinematic UI.

- [ ] **Step 4: Run transport test and firmware build**

Run: `./scripts/run_host_native_lesson_cinematic_display_transport_test.sh && cmake --build build --target app -j 8`

Expected: PASS and firmware links.

- [ ] **Step 5: Commit display transport**

```bash
git add main/display/lcd_display.* main/boards/lcdwiki-es3c35p/lcdwiki-es3c35p.cc tests/native/lesson_cinematic_display_transport_test.cc scripts/run_host_native_lesson_cinematic_display_transport_test.sh
git commit -m "feat(firmware): add asynchronous cinematic LCD ownership"
```

### Task 7: Firmware RGB565 renderer integration and double buffering

**Ownership:** Firmware renderer/handler integration only.

**Files:**
- Modify: `main/lesson_flattened_cinematic_renderer.h`
- Modify: `main/lesson_flattened_cinematic_renderer.cc`
- Modify: `main/lesson_handler.cc`
- Modify: `tests/native/lesson_flattened_cinematic_renderer_test.cc`
- Modify: `tests/native/lesson_handler_host_test.cc`

- [ ] **Step 1: Replace the MJPEG timing test with failing RGB565 double-buffer tests**

```cpp
Require(renderer.Prepare(RgbConfig(), 0).accepted, "TRGB prepare reads frame zero");
Require(fake.allocations == 2, "renderer owns exactly two frame buffers");
Require(renderer.Start(2, "barn-listen", 0).accepted, "prepared cue starts");
Require(renderer.Tick(100).accepted, "next frame is queued at 10 fps");
Require(fake.read_indices == std::vector<std::size_t>({0, 1, 2}), "reads are sequentially prefetched");
Require(!fake.overwrote_owned_buffer, "DMA-owned front buffer is never reused");
```

Add late-read, CRC failure, queue timeout, stop/cancel, session replacement, loop cue, and LVGL cleanup cases.

- [ ] **Step 2: Run renderer and handler tests and verify RED**

Run: `./scripts/run_host_native_lesson_flattened_cinematic_renderer_test.sh && ./scripts/run_host_native_lesson_handler_test.sh`

- [ ] **Step 3: Extend prepare metadata and choose parser by media type**

Add `container_version`, `stored_width`, `stored_height`, `orientation`, and `frame_bytes` to `LessonFlattenedCinematicAssetConfig`. Handler exact-key validation requires them only for `application/vnd.tbot.rgb565-indexed`; the MP4 legacy path remains explicit.

- [ ] **Step 4: Implement TRGB production ops and double-buffer lifecycle**

Allocate two PSRAM buffers, open `LessonRgb565File`, preload frame zero, call `BeginLessonCinematic`, prefetch the next frame while the current buffer is DMA-owned, and swap only after completion. Remove JPEG decode from the S3 TRGB path. Keep measured read, CRC, queue, dropped-frame, and heap logs.

- [ ] **Step 5: Run all renderer/handler suites and app build**

Run:

```bash
./scripts/run_host_native_lesson_rgb565_stream_test.sh
./scripts/run_host_native_lesson_cinematic_display_transport_test.sh
./scripts/run_host_native_lesson_flattened_cinematic_renderer_test.sh
./scripts/run_host_native_lesson_cinematic_renderer_test.sh
./scripts/run_host_native_lesson_handler_test.sh
cmake --build build --target app -j 8
```

Expected: all PASS; app partition remains within size limit.

- [ ] **Step 6: Commit renderer integration**

```bash
git add main/lesson_flattened_cinematic_renderer.* main/lesson_handler.cc tests/native/lesson_flattened_cinematic_renderer_test.cc tests/native/lesson_handler_host_test.cc
git commit -m "feat(firmware): stream RGB565 cinematics at 10 fps"
```

### Task 8: Cross-repo fixtures, regression verification, and production HIL

**Ownership:** Root agent integrates; no worker deploys production independently.

**Files:**
- Update generated fixture files only through existing scripts.
- Modify: `docs/validation/tvideo-farm-software.md` where the new media contract is documented.

- [ ] **Step 1: Build a real farm cue locally and validate both outputs**

Run the focused backend renderer on `barn-listen`; verify MP4 with `ffprobe`, parse TRGB with the new TypeScript validator, and assert 13 frames, 1,300 ms, 307,200 bytes/frame, and panel-native rotation.

- [ ] **Step 2: Regenerate ESP and firmware fixtures**

Run: `python scripts/project_tvideo_farm_firmware_fixture.py` in the ESP server worktree, then run its cross-repo fixture tests. The generated start body must remain exactly `command`, `cueId`, and `commandSequenceId`.

- [ ] **Step 3: Run full focused software regression**

Backend:

```bash
npx vitest run src/lessons/derivatives src/lessons/authoring/lesson-authoring.flattened-derivatives.spec.ts src/lessons/lesson-manifest.logic.validation.spec.ts src/lessons/lesson-sd-pack.contract.spec.ts tests/tvideo-rgb565-derivatives.migration.spec.ts
npm run build
```

ESP server:

```bash
pytest -q tests/test_flattened_cinematic_contract.py tests/test_lesson_asset_cache.py tests/test_lesson_conversation_integration.py tests/test_tvideo_farm_cross_repo_fixture.py
```

Firmware: run every command listed in Task 7 Step 5.

- [ ] **Step 4: Deploy backend migration/render worker and generate all 19 device derivatives**

Use the existing production rollout mechanism. Do not print secrets. Poll readiness until all 19 cues have both preview and device outputs, then republish farm v7 without changing its authored source revision.

- [ ] **Step 5: Deploy ESP server behind the existing rollback image**

Persist a new tagged image and retain `local/tbot-server:vps-20260803090035-controlstartstrict` plus `/opt/tbot/releases/runtime-control-start-20260803090035` as rollback.

- [ ] **Step 6: App-flash only and run scoped HIL**

Flash `build/xiaozhi.bin` at app offset `0x20000` to `/dev/cu.usbmodem1101`. Create a fresh assignment only for device `91deb5af-c1c0-416b-956d-266d510eac5e`, nudge it inside the ESP container, and capture serial/server evidence.

- [ ] **Step 7: Enforce completion gates**

Require:

- 27/27 assets checksum-ready;
- every production cue starts and transitions;
- sustained 10 fps, SD read+CRC p99 below 70 ms;
- no consecutive dropped frames and no more than one drop per 300 frames;
- Google Live gentle coaching and physical actions continue;
- no parser, metadata, CRC, DMA ownership, watchdog, reset, or reboot errors;
- visual confirmation on the physical robot.

- [ ] **Step 8: Remove temporary timing noise and run final verification**

Keep bounded production metrics but remove per-frame diagnostic spam added during investigation. Run `git diff --check`, all focused suites, and the app build once more before reporting completion.
