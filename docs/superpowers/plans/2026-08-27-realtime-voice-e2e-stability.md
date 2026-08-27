# Realtime Voice E2E Stability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make ten consecutive production `Hi ESP` voice turns complete quickly and reliably, then survive a fifteen-minute idle soak without resets, stuck states, failed reconnects, or memory-allocation failures.

**Architecture:** Treat firmware serial, ESP server timing, and device state as one timestamped evidence stream. Fix memory headroom before latency: reserve only stacks that must survive fragmented SRAM, defer background asset traffic away from voice-critical phases, and preserve the existing WebSocket/audio protocol. Every behavior change starts with a failing contract or host-native test and ends with a production build, NVS-preserving flash, and physical rerun.

**Tech Stack:** ESP-IDF 5.5.4, C++ firmware, FreeRTOS tasks/queues, ESP WebSocket/TLS, WakeNet/AFE, Opus, Python pytest contract tests, pyserial, esptool.

---

### Task 1: Capture A Reproducible Physical Baseline

**Files:**
- Create: `scripts/realtime_voice_e2e_probe.py`
- Create: `tests/test_realtime_voice_e2e_probe.py`
- Evidence: `.codex_tmp/realtime-voice-e2e-<timestamp>/`

- [ ] **Step 1: Write failing parser tests**

Add fixtures covering `Wake word detected`, `SendStartListening`, first `audio_uplink_packet_queued`, `tts_start_received`, playback start, `tts_stop_received`, idle rearm, `ws_disconnect`, `heap_alloc_failed`, reset markers, and periodic heap metrics. Assert the parser emits one ordered turn record and rejects a turn missing TTS or idle rearm.

- [ ] **Step 2: Verify the parser tests fail**

Run: `python3 -m pytest tests/test_realtime_voice_e2e_probe.py -q`

Expected: FAIL because `scripts.realtime_voice_e2e_probe` does not exist.

- [ ] **Step 3: Implement the minimal probe**

Implement a pyserial capture that writes `firmware-serial.log`, accepts operator turn markers on stdin, emits `turns.json`, and returns non-zero when any accepted wake lacks uplink, TTS, playback, or idle rearm. Include latency fields for wake-to-listen, first-uplink, question-end-to-TTS, and TTS-to-idle.

- [ ] **Step 4: Verify parser behavior**

Run: `python3 -m pytest tests/test_realtime_voice_e2e_probe.py -q`

Expected: PASS.

- [ ] **Step 5: Capture the current flashed baseline**

Run the probe on `/dev/cu.usbmodem1101` for three operator turns plus two idle minutes. Record current firmware hashes, production URLs, internal free SRAM, largest internal block, reconnect count, and any `heap_alloc_failed` lines.

- [ ] **Step 6: Commit the probe**

Run: `git add scripts/realtime_voice_e2e_probe.py tests/test_realtime_voice_e2e_probe.py && git commit -m "test(voice): add physical realtime turn probe"`

### Task 2: Restore Internal SRAM Headroom Without Reintroducing Heartbeat Fragmentation

**Files:**
- Modify: `main/application.cc`
- Modify: `main/application.h`
- Modify: `tests/test_internal_ram_guardrails.py`
- Modify: `tests/test_lesson_network_stack_contract.py`
- Modify: `tests/test_tbot_connect_runtime_fsm_contract.py`
- Test: `tests/test_realtime_voice_state.py`

- [ ] **Step 1: Convert current evidence into a failing memory contract**

Require the heartbeat worker to be created before AFE prewarm/TLS fragmentation, remain persistent across reconnects, and avoid a second 8,192-byte global static stack reservation. Require the queue to remain one-slot and forbid transient heartbeat workers in `DispatchDeviceHeartbeat()`.

- [ ] **Step 2: Verify the memory contract fails against the current static-stack patch**

Run: `python3 -m pytest tests/test_internal_ram_guardrails.py tests/test_lesson_network_stack_contract.py tests/test_tbot_connect_runtime_fsm_contract.py -q`

Expected: FAIL because `heartbeat_task_stack` currently consumes a permanent 8,192-byte internal DRAM reservation.

- [ ] **Step 3: Implement early persistent heartbeat allocation**

Create the heartbeat queue and internal-capability task once during application initialization, before network TLS and AFE prewarm. Keep the task blocked on its queue until claimed/online. `StartHeartbeat()` should only arm the timer and must not allocate a queue or task. Do not reduce the measured 8,192-byte stack requirement or move a TLS/NVS worker stack to SPIRAM.

- [ ] **Step 4: Run focused connection and memory tests**

Run: `python3 -m pytest tests/test_internal_ram_guardrails.py tests/test_lesson_network_stack_contract.py tests/test_tbot_connect_runtime_fsm_contract.py tests/test_tbot_connect_review_fixes.py tests/test_realtime_voice_state.py -q`

Expected: PASS with no collection errors or warnings introduced by the change.

- [ ] **Step 5: Build the production image with an isolated SDK config**

Use `build-production-realtime-voice/sdkconfig`, verify `CONFIG_WEBSOCKET_URL="wss://esp.tjbot.vn/tbot/v1/"`, `CONFIG_OTA_URL="https://esp.tjbot.vn/tbot/ota/"`, `CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y`, and local endpoint mode disabled. Build with toolchain `esp-14.2.0_20260121`.

- [ ] **Step 6: Flash without NVS and capture a boot memory gate**

Flash offsets `0x0`, `0x8000`, `0xd000`, `0x20000`, and `0x800000` only. Require successful Wi-Fi, production WebSocket, wake rearm, no reset, no heartbeat allocation failure, and enough largest internal block to complete one HTTPS asset fetch and one wake reconnect.

- [ ] **Step 7: Commit the memory fix**

Run: `git add main/application.cc main/application.h tests/test_internal_ram_guardrails.py tests/test_lesson_network_stack_contract.py tests/test_tbot_connect_runtime_fsm_contract.py tests/test_realtime_voice_state.py && git commit -m "fix(voice): allocate heartbeat worker before heap fragmentation"`

### Task 3: Keep Background Asset Sync Out Of Voice-Critical Windows

**Files:**
- Modify: `main/application.cc`
- Modify: `main/application.h`
- Modify: `tests/test_lesson_sd_sync_worker_contract.py`
- Modify: `tests/test_lesson_passive_websocket_contract.py`
- Modify: `tests/test_realtime_voice_state.py`

- [ ] **Step 1: Write failing scheduling tests**

Assert lesson asset sync does not start while a voice connect is active, while listening/speaking, during reconnect backoff, or when the largest internal block is below the measured TLS requirement. Assert deferred sync resumes only after idle wake rearm and a stable passive WebSocket.

- [ ] **Step 2: Verify the tests fail**

Run: `python3 -m pytest tests/test_lesson_sd_sync_worker_contract.py tests/test_lesson_passive_websocket_contract.py tests/test_realtime_voice_state.py -q`

Expected: FAIL on at least the low-largest-block and reconnect exclusions.

- [ ] **Step 3: Add a single asset-sync admission guard**

Centralize the voice state, reconnect state, lesson state, and largest-internal-block checks used before `BeginLessonAssetSyncQuiet()`. Defer rather than discard work and preserve the existing sync retry path.

- [ ] **Step 4: Verify focused tests**

Run the three test files from Step 2 plus `tests/test_internal_ram_guardrails.py`.

Expected: PASS.

- [ ] **Step 5: Build, flash, and verify concurrent pressure**

After boot, allow one asset fetch, trigger `Hi ESP`, and verify the voice turn preempts/defer sync without `heap_alloc_failed`, WebSocket drop, or stuck wake state.

- [ ] **Step 6: Commit the scheduling fix**

Run: `git add main/application.cc main/application.h tests/test_lesson_sd_sync_worker_contract.py tests/test_lesson_passive_websocket_contract.py tests/test_realtime_voice_state.py && git commit -m "fix(voice): defer asset sync during realtime turns"`

### Task 4: Close Any Voice State Or Reconnect Failures Found By The Probe

**Files:**
- Modify as evidence requires: `main/application.cc`
- Modify as evidence requires: `main/application.h`
- Modify as evidence requires: `main/audio/audio_service.cc`
- Modify as evidence requires: `main/protocols/websocket_protocol.cc`
- Test: `tests/test_realtime_voice_state.py`
- Test: `tests/test_wake_word_live_uplink_contract.py`
- Test: `tests/test_wake_word_lifecycle_contract.py`

- [ ] **Step 1: Select the first remaining failed turn invariant**

Use `turns.json` and raw serial to identify one earliest boundary failure: wake-to-listen, uplink, WebSocket continuity, incoming TTS, playback, stop, or idle rearm. Do not bundle independent failures.

- [ ] **Step 2: Write one failing regression test**

Add the smallest contract or host-native test reproducing the selected failure and run it to confirm RED.

- [ ] **Step 3: Implement the minimal root-cause fix**

Change only the owning state transition or resource boundary. Preserve generation guards, online intent, reconnect backoff, and stale-frame rejection.

- [ ] **Step 4: Verify GREEN and rerun the physical turn**

Run the focused test, the three voice contract suites listed above, build production, flash without NVS, and rerun the exact failed turn.

- [ ] **Step 5: Repeat only for independently evidenced failures**

Stop and reassess architecture if three distinct fix attempts fail to produce a complete turn.

- [ ] **Step 6: Commit each independent fix**

Use one `fix(voice): ...` commit per root cause.

### Task 5: Run The Ten-Turn Latency And Stability Gate

**Files:**
- Evidence: `.codex_tmp/realtime-voice-e2e-<timestamp>/firmware-serial.log`
- Evidence: `.codex_tmp/realtime-voice-e2e-<timestamp>/turns.json`
- Evidence: `.codex_tmp/realtime-voice-e2e-<timestamp>/summary.md`

- [ ] **Step 1: Record candidate identity**

Record Git HEAD, dirty diff hash if any, five image SHA-256 values, board MAC, production URLs, and NVS-preserving flash command.

- [ ] **Step 2: Run ten consecutive operator turns**

Use short Vietnamese questions and mark question end consistently. Do not reboot between turns.

- [ ] **Step 3: Evaluate the acceptance gate**

Require first-utterance wake success at least 9/10, complete TTS plus idle rearm for every accepted wake, first response audio under 2,000 ms for at least 9/10 completed turns, and zero panic/reset/stuck-listening/unexpected-disconnect/failed-reconnect/allocation-failure events.

- [ ] **Step 4: Run the fifteen-minute soak**

Require wake detector running, passive WebSocket healthy, reconnect count stable, queues bounded, no progressive internal-memory decline, and one successful wake turn after the soak.

- [ ] **Step 5: Write the final evidence summary**

Include per-turn latency, p50/p95/max, wake pass rate, reconnect/drop/reset counts, heap minima, largest-block minimum, task stack minima, and explicit PASS/FAIL for every spec criterion.

### Task 6: Final Verification And Handoff

**Files:**
- Modify if needed: `docs/superpowers/specs/2026-08-27-realtime-voice-e2e-stability-design.md`
- Evidence: final run directory from Task 5

- [ ] **Step 1: Run the full relevant software suite**

Run all modified test files plus the wake host-native lifecycle script and `git diff --check`.

- [ ] **Step 2: Verify the final production artifact**

Confirm build exit code zero, production config values, image hashes, successful esptool hash verification, and post-flash boot markers.

- [ ] **Step 3: Review residual risk**

Document any server-side latency outlier, environmental wake miss, or untested failure mode separately from firmware acceptance.

- [ ] **Step 4: Present integration options**

Use the finishing-a-development-branch workflow only after all acceptance criteria pass.

