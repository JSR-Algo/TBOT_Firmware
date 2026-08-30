# BluFi Lifecycle Race Closure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close stale BLE teardown and fallback advertising callback races before merging the remaining Wi-Fi scan work into firmware `main`.

**Architecture:** Keep lifecycle ownership in `blufi.cpp`. Serialize station-association teardown with `provisioning_finalization_mutex_`, and route default BluFi advertising configuration/start callbacks through FIFO epoch ownership just like compact raw advertising callbacks.

**Tech Stack:** C++17, ESP-IDF Bluedroid GAP/BluFi, pytest source-contract tests, ESP32-S3 hardware E2E.

---

### Task 1: Lock station-association BLE release

**Files:**
- Modify: `tests/test_blufi_provisioning_stability.py`
- Modify: `main/boards/common/blufi.cpp`

- [ ] Add a source-contract test asserting `provisioning_finalization_mutex_` is locked before the generation check and remains in scope through `deinit()`.
- [ ] Run `python3 -m pytest -q tests/test_blufi_provisioning_stability.py -k release_ble` and confirm the new assertion fails.
- [ ] Add `std::lock_guard<std::mutex> finalization_lock(provisioning_finalization_mutex_);` at the start of `ReleaseBleForStationAssociation()` before reading `setup_generation_`.
- [ ] Re-run the focused test and confirm it passes.

### Task 2: Own fallback advertising callbacks

**Files:**
- Modify: `tests/test_wifi_provisioning_brand.py`
- Modify: `main/boards/common/blufi.cpp`

- [ ] Add a source-contract test requiring a FIFO for default advertising data callback epochs, queueing in `FallbackToDefaultBlufiAdvertising()`, and transfer to the start-complete FIFO before forwarding `ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT` to ESP-IDF.
- [ ] Run `python3 -m pytest -q tests/test_wifi_provisioning_brand.py -k fallback` and confirm the test fails.
- [ ] Add `tbot_default_adv_data_callback_epochs`, enqueue the active epoch in fallback, and add a helper that pops that epoch on structured ADV-data completion and queues the matching start epoch before calling `esp_blufi_gap_event_handler()`.
- [ ] Re-run the focused test and confirm it passes.

### Task 3: Integrate Wi-Fi scan mode safely

**Files:**
- Modify: `main/boards/common/blufi.cpp`
- Modify: `tests/test_blufi_wifi_scan_contract.py`

- [ ] Cherry-pick `acc8c23` and resolve only scan initialization/failure-log hunks on current `main`.
- [ ] Preserve passive scanning, bounded candidate ownership, deferred list delivery, advertising epochs, and lifecycle mutex code from current `main`.
- [ ] Cherry-pick `83e87b7` for BluFi DMA heap diagnostics.
- [ ] Run `python3 -m pytest -q tests/test_blufi_wifi_scan_contract.py tests/test_blufi_provisioning_stability.py tests/test_wifi_provisioning_brand.py` and confirm all pass.

### Task 4: Verify and deliver

**Files:**
- Verify only; no planned source changes.

- [ ] Run the full firmware pytest suite.
- [ ] Build the ESP32-S3 production image and flash the app partition without erasing NVS.
- [ ] Build/install Android mobile `main`, connect to the attached phone, and run one physical Wi-Fi provisioning E2E cycle.
- [ ] Verify robot logs show BLE discovery, Wi-Fi scan/list delivery, credential receipt, station connection, IP acquisition, and setup completion without panic or stale lifecycle teardown.
- [ ] Remove obsolete worktrees after all checks pass.
