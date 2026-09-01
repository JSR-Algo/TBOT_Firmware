# BluFi Poller Dispatch And Completion Ordering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep raw scan start on the Application task and restore fail-closed logical-before-physical completion ordering.

**Architecture:** Add an exact revision enqueue transaction to the durable retry state and make every poll callback use it. Split logical completion into prepare, retained commit, and final release so the physical lease is released only after logical commit while pending promotion waits for both sides.

**Tech Stack:** C++17, ESP-IDF Wi-Fi/FreeRTOS, native host tests, pytest source contracts, Clang sanitizers, Ninja/Xtensa.

---

### Task 1: Application-only durable dispatch

**Files:**
- Modify: `main/boards/common/blufi_wifi_scan_retry_state.h`
- Modify: `main/boards/common/blufi.cpp`
- Modify: `main/boards/common/blufi.h`
- Modify: `tests/native/blufi_wifi_scan_retry_state_host_test.cc`
- Modify: `tests/test_blufi_wifi_scan_contract.py`

- [ ] Add a failing native integration model proving a worker poll only queues an exact revision, an injected scheduler throw remains retryable, the Application executor starts exactly once, and a replaced revision callback is stale.
- [ ] Run the retry-state native script and focused contract test; confirm failure because the poll callback still calls `TryStartOwnedWifiScanNow` directly.
- [ ] Add exact enqueue reservation/commit/cancel operations and route initial signals plus manager polls through one noexcept BluFi enqueue helper.
- [ ] Run the focused tests and confirm the poller contains no raw start/driver path.

### Task 2: Retained logical-before-physical completion

**Files:**
- Modify: `main/boards/common/blufi_wifi_scan_controller.h`
- Modify: `main/boards/common/blufi.cpp`
- Modify: `tests/native/blufi_wifi_scan_controller_host_test.cc`
- Modify: `tests/test_blufi_wifi_scan_contract.py`

- [ ] Add failing deterministic tests for prepare/commit/release ordering, competitor blocking, pending invisibility, and physical finish failure retaining both sides for recovery.
- [ ] Run controller native and contract tests; confirm failure because physical finish currently precedes logical finish.
- [ ] Implement logical prepare, retained commit, recovery retention, and final pending release; serialize logical commit before exact physical finish and publish pending only after success on both sides.
- [ ] Run focused tests and confirm all completion ordering assertions pass.

### Task 3: Verification and commit

**Files:**
- Verify every file modified in Tasks 1-2.

- [ ] Run the nine-file BluFi/WiFi pytest scope.
- [ ] Run all six native scripts with normal, ASan/UBSan, and TSan variants.
- [ ] Build `esp-idf/esp-wifi-connect/libesp-wifi-connect.a` and the BluFi Xtensa object.
- [ ] Run `git diff --check`, obtain independent review, fix all Critical/Important findings, and commit without merging or flashing.
