# Cardputer Provisioning Transaction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Cardputer Wi-Fi provisioning transactional, force the exact requested credentials, and complete Application setup exactly once after success.

**Architecture:** Bind one provisional `SsidManager` transaction to each credential intent revision. The connection worker restarts station mode under the global lifecycle lease and injects only the requested SSID/password, then commits or rolls back the exact transaction. Successful UI delivery schedules the existing generation-guarded Application provisioning promotion path.

**Tech Stack:** ESP-IDF C++, FreeRTOS workers, host-native C++ tests, pytest source contracts.

---

### Task 1: Credential Transaction Ownership

**Files:**
- Modify: `main/boards/m5stack-cardputer-adv/cardputer_wifi_deferred_intent_state.h`
- Modify: `main/boards/m5stack-cardputer-adv/m5stack_cardputer_adv.cc`
- Modify: `main/boards/m5stack-cardputer-adv/wifi_config_ui.cc`
- Test: `tests/native/blocking_wifi_scan_lease_host_test.cc`

- [x] Add failing tests proving a revision owns one transaction ID, supersession/cancel returns the stale transaction for rollback, failure rolls back, and matching success commits once.
- [x] Replace worker `AddSsid()` with `BeginSsidTransaction()`, exact commit/rollback, and remove UI success persistence.
- [x] Run the native blocking worker suite through normal and sanitizers.

### Task 2: Exact Credential Station Attempt

**Files:**
- Modify: `components/esp-wifi-connect/include/wifi_manager.h`
- Modify: `components/esp-wifi-connect/wifi_manager.cc`
- Modify: `components/esp-wifi-connect/include/wifi_station.h`
- Modify: `components/esp-wifi-connect/wifi_station.cc`
- Test: `tests/native/wifi_manager_recovery_host_test.cc`

- [x] Add failing tests for active/disconnected corrected credentials, dynamic wrong-SSID connection, and stronger old AP ordering.
- [x] Add a lifecycle-leased manager entry point that stops any active station, starts a fresh station generation, and injects the exact requested credentials before waiting.
- [x] Verify only the desired SSID can satisfy success and mismatches trigger retry/rollback.

### Task 3: Generation-Bound Application Completion

**Files:**
- Modify: `main/application.h`
- Modify: `main/application.cc`
- Modify: `main/boards/m5stack-cardputer-adv/m5stack_cardputer_adv.cc`
- Test: `tests/test_tbot_connect_review_fixes.py`
- Test: `tests/test_blufi_wifi_scan_contract.py`

- [x] Add a failing contract proving Cardputer success schedules generation-bound promotion after setup UI ownership is released.
- [x] Expose a narrow Application method that reuses `PromoteFromWifiConfigAfterProvisioning()` under the expected setup generation and is exactly-once for a Cardputer UI generation.
- [x] Ensure reconnect `AlreadyActive` uses the same completion without requiring another network event.

### Task 4: Full Verification And Follow-Up Commit

- [x] Run both native sanitizer runners.
- [x] Run focused provisioning, Application, and Wi-Fi pytest gates.
- [x] Rebuild Cardputer, Wi-Fi manager/station, and Application objects.
- [ ] Run `git diff --check`, request review, fix all Critical/Important findings, and create a new follow-up commit without amend.
