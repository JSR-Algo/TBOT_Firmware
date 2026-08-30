# BLE Searching Device Copy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show `Đang tìm kiếm thiết bị...` on the robot only while its BLE Wi-Fi setup service is advertising.

**Architecture:** Keep `TbotConnectState` as the source of truth and give `BLE_SETUP_ADVERTISING` distinct copy from the existing open-app states. Extend the locale contract so both language assets remain complete, then verify the mapper, build, physical display, and Android-to-ESP provisioning flow.

**Tech Stack:** ESP-IDF C++, JSON locale assets, pytest contract tests, Ninja, esptool, ADB, ESP serial logging.

---

### Task 1: Lock the BLE-only copy contract

**Files:**
- Modify: `tests/test_tbot_connect_runtime_fsm_contract.py`

- [ ] **Step 1: Write the failing contract test**

Add a `SEARCHING_FOR_DEVICE` locale requirement and assert that the
`BLE_SETUP_ADVERTISING` row uses `Searching for device...`, while
`WIFI_NOT_CONFIGURED` and `AP_SETUP_ACTIVE` keep `Open TBot app`.

```python
"SEARCHING_FOR_DEVICE": ("Searching for device...", "Đang tìm kiếm thiết bị..."),
```

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```bash
pytest -q tests/test_tbot_connect_runtime_fsm_contract.py
```

Expected: failure because the new locale key and BLE-specific state copy do not yet exist.

### Task 2: Implement the localized BLE advertising copy

**Files:**
- Modify: `main/tbot_connect_state.h`
- Modify: `main/assets/locales/en-US/language.json`
- Modify: `main/assets/locales/vi-VN/language.json`

- [ ] **Step 1: Add locale strings**

```json
"SEARCHING_FOR_DEVICE": "Searching for device..."
```

```json
"SEARCHING_FOR_DEVICE": "Đang tìm kiếm thiết bị..."
```

- [ ] **Step 2: Change only the BLE advertising state**

Set `TbotConnectState::BLE_SETUP_ADVERTISING.screen_text` to
`Searching for device...`. Leave the other `Open TBot app` rows unchanged.

- [ ] **Step 3: Run focused tests and verify GREEN**

Run:

```bash
pytest -q tests/test_tbot_connect_runtime_fsm_contract.py tests/test_tbot_connect_review_fixes.py tests/test_wifi_board_provisioning.py
```

Expected: all tests pass.

### Task 3: Build, flash, and verify on hardware

**Files:**
- Build output: `build-blufi-gatt-diagnostics/xiaozhi.bin`

- [ ] **Step 1: Build firmware**

Run:

```bash
/opt/homebrew/bin/ninja -C build-blufi-gatt-diagnostics
```

Expected: successful build and an app image that fits the configured partition.

- [ ] **Step 2: Flash only the app partition**

Use direct esptool to write `xiaozhi.bin` at `0x20000`, preserving NVS and pairing state.

- [ ] **Step 3: Verify the physical BLE state**

Double-click BOOT once. Confirm sanitized ESP logs contain successful BLUFI advertising and the robot display shows exactly `Đang tìm kiếm thiết bị...`.

- [ ] **Step 4: Continue phone-driven E2E**

Use ADB to retry discovery and provisioning while recording package-specific Android logs and ESP serial. Confirm GATT connection, service discovery, MTU, Wi-Fi credential handoff, Wi-Fi IP acquisition, BLE teardown, and robot transition out of `wifi_configuring`.

- [ ] **Step 5: Repeat the requested reliability cycles**

Complete at least three cancel/reconnect cycles and three Wi-Fi-change cycles without GATT 133, stale navigation, stuck initialization, secret leakage, or NVS erasure.

### Task 4: Final verification

**Files:**
- No production file changes unless a newly reproduced defect has a confirmed root cause and a failing test.

- [ ] **Step 1: Run firmware and mobile regression gates**

Run the focused firmware contract suite, native lifecycle/concurrency tests, mobile pairing regression suite, TypeScript, lint, Android build, and firmware build.

- [ ] **Step 2: Review diffs and sanitized evidence**

Confirm only intended changes are present and no Wi-Fi credentials, device identifiers, MAC addresses, tokens, or serials appear in committed source or QA output.

No git commit is included because the user has not requested one.
