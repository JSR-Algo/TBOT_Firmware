# BluFi Advertising Reconnect Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix asynchronous BluFi advertising setup and prove three consecutive Android GATT reconnect cycles.

**Architecture:** Register a TBOT GAP callback that delegates normal BluFi handling to ESP-IDF, owns only raw advertising completion coordination, and starts compact advertising after both payloads are ready. Preserve the existing fallback and avoid all NVS mutations.

**Tech Stack:** ESP-IDF 5.5.4, ESP32-S3 Bluedroid/BluFi, pytest source-contract tests, esptool, Android ADB.

---

### Task 1: Lock The Advertising Ordering Contract

**Files:**
- Modify: `tests/test_wifi_provisioning_brand.py`

- [ ] Add a test requiring a TBOT GAP callback, forwarding to `esp_blufi_gap_event_handler`, handling both raw completion events, and forbidding `esp_ble_gap_start_advertising` inside `StartTbotBlufiAdvertising`.
- [ ] Run `pytest -q tests/test_wifi_provisioning_brand.py -k advertising` and confirm the new assertion fails for eager advertising.

### Task 2: Gate Advertising On Payload Completion

**Files:**
- Modify: `main/boards/common/blufi.cpp`

- [ ] Add resettable ADV/scan-response pending state and static advertising parameters.
- [ ] Add a GAP callback that forwards the event to BluFi, validates both raw completion statuses, falls back on failure, and starts compact advertising exactly once when both complete.
- [ ] Register the wrapper callback and make `StartTbotBlufiAdvertising` configure payloads without starting early.
- [ ] Run the focused test and confirm GREEN.
- [ ] Run the related provisioning, stability, security, and redaction suites.

### Task 3: Build And Flash Safely

**Files:**
- Use: `sdkconfig.defaults.blufi-gatt-diagnostics`

- [ ] Build the LCDWiki ESP32-S3 firmware using the existing ESP-IDF 5.5.4 environment.
- [ ] Flash only the application image at `0x20000`; do not erase flash or write NVS.
- [ ] Verify the flashed image hash.

### Task 4: Hardware Reconnect Demonstration

**Files:**
- No repository changes.

- [ ] Enter Wi-Fi change mode with a BOOT double-click.
- [ ] Confirm Android discovers the TBOT advertisement.
- [ ] Repeat three times without robot reboot: connect, discover service `0xFFFF`, disconnect locally, and reconnect.
- [ ] Capture safe timestamps and verify three clean GATT closes with no credential-bearing output.

