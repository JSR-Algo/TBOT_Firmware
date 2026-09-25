# Opus PSRAM Provisioning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate internal-SRAM contention between the Opus audio worker and Bluedroid so repeated Android-to-robot Wi-Fi provisioning completes without BOOT presses or heap panics.

**Architecture:** Allocate the Opus worker through ESP-IDF's capability-aware task API in PSRAM and delete it with the matching capability-aware API. Remove the obsolete 28 KiB internal reservation while retaining the post-BLE internal/DMA headroom reservation used during Wi-Fi association and audio rearm.

**Tech Stack:** ESP-IDF 5.x, FreeRTOS capability-aware task APIs, C++ firmware, pytest contract tests, Android React Native app, ADB, ESP32-S3 serial/flash tooling.

---

### Task 1: Add the Opus PSRAM regression contract

**Files:**
- Modify: `tests/test_audio_rearm_transaction_contract.py`
- Test: `tests/test_audio_rearm_transaction_contract.py`

- [ ] Add a test that extracts the Opus branch of `AudioService::CreateAudioWorker` and requires `xTaskCreateWithCaps`, `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`, and `vTaskDeleteWithCaps`.
- [ ] Update the provisioning reservation contract so it rejects `ReserveWifiStationAssociationStack` and still requires post-BLE network headroom before station association.
- [ ] Run `pytest -q tests/test_audio_rearm_transaction_contract.py` and confirm failure because production still creates Opus with `xTaskCreate` and reserves internal SRAM.

### Task 2: Move the Opus worker stack to PSRAM

**Files:**
- Modify: `main/audio/audio_service.cc`
- Modify: `main/audio/audio_service.h`
- Modify: `main/boards/common/wifi_board.cc`
- Modify: `main/boards/common/blufi.cpp`
- Test: `tests/test_audio_rearm_transaction_contract.py`

- [ ] Replace Opus `xTaskCreate` with `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`.
- [ ] Replace the Opus worker's self-delete with `vTaskDeleteWithCaps(NULL)` after clearing its task handle.
- [ ] Remove the 28 KiB reservation API, state, allocation, release calls, and rollback ordering constraints.
- [ ] Make post-BLE network-headroom reservation ownership depend only on the provisioning generation.
- [ ] Run `pytest -q tests/test_audio_rearm_transaction_contract.py` and confirm the new contract passes.

### Task 3: Run automated firmware and mobile verification

**Files:**
- Verify: `tests/`
- Verify: `../tbot-mobile/tests/`

- [ ] Run the complete firmware pytest suite.
- [ ] Run the native firmware tests configured by the repository.
- [ ] Build the ESP32-S3 firmware image.
- [ ] Run targeted mobile provisioning tests, TypeScript typecheck, targeted ESLint, and Android Gradle build.

### Task 4: Flash and execute physical E2E validation

**Files:**
- Flash: built application image to the connected ESP32-S3
- Install: current Android APK only if mobile artifacts changed after the prior successful install

- [ ] Flash the new firmware while preserving NVS and device assets, reboot, and confirm normal saved-Wi-Fi/audio startup.
- [ ] Perform one cancel-during-scan attempt and verify automatic recovery without BOOT.
- [ ] Perform four complete setup/connect cycles on the target SSID.
- [ ] For every successful cycle, verify BLE scan and GATT, BLE teardown, network-headroom reservation, exact SSID/IP, audio worker and wake-word rearm, fresh HTTP 204 heartbeat, and online app state.
- [ ] Reject and debug any cycle containing heap allocation failure, watchdog, assertion, panic, Guru Meditation, stale generation completion, or a stuck app screen.

### Task 5: Final evidence review

**Files:**
- Review: working-tree diff only; do not commit, merge, push, or delete worktrees

- [ ] Re-run affected automated tests after the final physical fix.
- [ ] Inspect final firmware and mobile diffs for credential leakage and unintended changes.
- [ ] Report exact test/build/E2E counts and residual hardware/network risks without claiming absolute bug freedom.
