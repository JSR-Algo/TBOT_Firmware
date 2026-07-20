# WiFi Config Entry Transaction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make BLUFI configuration entry fail before station, state, or configuration-mode mutation and provide generation-safe rollback for setup failures.

**Architecture:** `WifiBoard::StartWifiConfigMode()` becomes the single transaction owner. BLUFI reservation, audio quiesce, token commit, and required stack initialization complete before common entry side effects; exact-token rollback is allowed only when failed initialization proves BLE is off.

**Tech Stack:** C++17, ESP-IDF, native C++ lifecycle harness, pytest source-contract tests.

---

### Task 1: Lock the entry ordering with failing tests

**Files:**
- Modify: `tests/test_wifi_board_provisioning.py`
- Modify: `tests/native/wake_word_lifecycle_gate_test.cc`
- Modify: `tests/test_provisioning_success_teardown_contract.py`

- [ ] Add a source contract requiring reserve, Begin, Commit, and successful conditional init before `StopStation`, `in_config_mode_`, and `SetDeviceState`.
- [ ] Require timeout and delayed/direct entry callers to call `StartWifiConfigMode()` without stopping the station first.
- [ ] Extend the native active-completion scenario with counters proving Begin, station stop, config flag, and device-state mutation remain untouched.
- [ ] Run the focused pytest and native lifecycle commands and confirm failures identify the current pre-mutation ordering.

### Task 2: Add exact binding abort support

**Files:**
- Modify: `main/audio/provisioning_session_binding.h`
- Modify: `main/boards/common/blufi.h`
- Modify: `main/boards/common/blufi.cpp`
- Test: `tests/native/wake_word_lifecycle_gate_test.cc`

- [ ] Add `ProvisioningSessionBinding::Clear(Token)` that clears only a matching valid token while no completion or reservation guard is active.
- [ ] Expose the operation through BLUFI with an exact-token name.
- [ ] Add native tests for matching clear, stale-token rejection, and unchanged binding on rejection.
- [ ] Run the native lifecycle test and confirm it passes.

### Task 3: Centralize and make BLUFI entry transactional

**Files:**
- Modify: `main/boards/common/wifi_board.cc`
- Test: `tests/test_wifi_board_provisioning.py`
- Test: `tests/test_provisioning_success_teardown_contract.py`

- [ ] Remove caller-side station/timer mutations from timeout, immediate, and delayed entry paths.
- [ ] In `StartWifiConfigMode()`, complete BLUFI reserve, Begin, Commit, and required init before common visible mutations.
- [ ] On Commit failure, rearm the exact generation. On init failure, clear and rearm only when `GetBleState() == kOff`; otherwise retain fail-closed ownership and log.
- [ ] Move common timer stop, station stop, protocol reset, config flag, device state, and notification after successful preflight while preserving non-BLUFI behavior.
- [ ] Run focused Python and native gates until green.

### Task 4: Verify and commit

**Files:**
- Verify all modified production and test files.

- [ ] Run focused provisioning contracts and both native lifecycle/transition gates.
- [ ] Run the full pytest suite, recording only the known local `sdkconfig` OTA seed failure.
- [ ] Run the suite excluding that local-config test, lesson coverage, and the ESP-IDF build.
- [ ] Run shell syntax checks, `git diff --check`, inspect the final diff, and commit without amending.
