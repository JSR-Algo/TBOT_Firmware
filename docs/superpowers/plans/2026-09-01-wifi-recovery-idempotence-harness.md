# Wi-Fi Recovery Idempotence And Behavioral Harness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make recovery restore retry-safe and proof-gated, then execute the production manager recovery state machine in a deterministic sanitizer host harness.

**Architecture:** A shared injectable restorer normalizes the Wi-Fi driver to stopped state before fully rebuilding Station or Config AP runtime settings. Recovery claims carry owner session/enable state, and exact proof completion alone re-enables scans. A compile-time test seam injects task/scanner/executor dependencies into the real manager implementation without duplicating its decisions.

**Tech Stack:** C++17, ESP-IDF Wi-Fi/FreeRTOS APIs, pytest contracts, Clang ASan/UBSan/TSan.

---

### Task 1: Retry-Safe Radio Restorer

**Files:**
- Create: `components/esp-wifi-connect/include/wifi_radio_recovery_restorer.h`
- Create: `components/esp-wifi-connect/wifi_radio_recovery_restorer.cc`
- Create: `tests/native/wifi_radio_recovery_restorer_host_test.cc`
- Create: `scripts/run_host_native_wifi_radio_recovery_restorer_test.sh`
- Modify: `components/esp-wifi-connect/CMakeLists.txt`

- [ ] Write failing native tests that inject each driver-stage failure and require the next attempt to call `stop` then the complete Station or Config AP sequence.
- [ ] Run `bash scripts/run_host_native_wifi_radio_recovery_restorer_test.sh`; expect compilation/test failure because the restorer does not exist.
- [ ] Implement `WifiRadioRecoveryRestorer::RestoreStation` and `RestoreConfigAp` using an injected `Driver` table, with stage-specific benign stop handling only.
- [ ] Re-run the native test and all supported sanitizers; expect every variant to pass.

### Task 2: Proof-Gated Scanner Recovery

**Files:**
- Modify: `components/esp-wifi-connect/include/wifi_station.h`
- Modify: `components/esp-wifi-connect/wifi_station.cc`
- Modify: `components/esp-wifi-connect/include/wifi_configuration_ap.h`
- Modify: `components/esp-wifi-connect/wifi_configuration_ap.cc`
- Modify: `components/esp-wifi-connect/include/wifi_manager.h`
- Modify: `components/esp-wifi-connect/wifi_manager.cc`
- Modify: `tests/native/wifi_radio_recovery_restorer_host_test.cc`
- Modify: `tests/test_blufi_wifi_scan_contract.py`

- [ ] Add failing tests requiring claims to capture `scan_session_id` and active scan state, immediately gate active scans, and keep them gated after restore failure or exact-proof failure.
- [ ] Run focused native/pytest tests and confirm failures point to current unconditional scan enablement.
- [ ] Pass the claim into restore, use the shared restorer, and move scan re-enablement into successful exact `CompleteScanRecovery` only.
- [ ] Run focused tests and confirm all owner/stage/proof cases pass.

### Task 3: Production Manager Behavioral Harness

**Files:**
- Modify: `components/esp-wifi-connect/include/wifi_manager.h`
- Modify: `components/esp-wifi-connect/wifi_manager.cc`
- Create: `tests/native/wifi_manager_recovery_host_test.cc`
- Create: `tests/native_stubs/wifi_manager_recovery/*`
- Create: `scripts/run_host_native_wifi_manager_recovery_test.sh`
- Create: `tests/test_wifi_manager_recovery_native.py`

- [ ] Add a failing harness that compiles actual `wifi_manager.cc` and injects task operations, scanner hooks, executor results, and target-start hooks.
- [ ] Verify RED for task-create retry, duplicate/new debt, callback-vs-claim, partial restore retry, exact-proof failure, both pending transitions, stale generation, active flags, and lock/callback order.
- [ ] Add `TBOT_WIFI_MANAGER_TESTING` dependency setters/state snapshots and retain scanner setup after task creation failure so Initialize retries that stage only.
- [ ] Run normal, ASan/UBSan, and TSan variants; expect all production-path scenarios to pass.

### Task 4: Verification And Commit

**Files:**
- Verify all files changed by Tasks 1-3.

- [ ] Run `python3 -m pytest -q tests/test_blufi*.py tests/test_wifi*.py`.
- [ ] Run all Wi-Fi native restorer, manager, executor, and coordinator scripts.
- [ ] Build `esp-idf/esp-wifi-connect/libesp-wifi-connect.a` with the ESP-IDF v5.5.2 environment and remove the temporary `managed_components` symlink.
- [ ] Run `git diff --check` and confirm no build artifacts or dependency lock changes.
- [ ] Commit implementation as `fix(wifi): make scan recovery retries idempotent` and report both commit SHAs and verification evidence.
