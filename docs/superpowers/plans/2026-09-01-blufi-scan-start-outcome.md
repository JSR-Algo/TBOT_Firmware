# BluFi Scan Start Outcome Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent exception rollback from interleaving physical/logical commits or resurrecting a completed/released BluFi scan lease.

**Architecture:** Add an exact no-throw start outcome and route all start commit pairs through one submission-mutex helper. Keep ownership finalization inside the noexcept start transaction; guard response, pending-scan, completion, recovery, and watchdog follow-up separately after the authoritative outcome is established.

**Tech Stack:** C++17, ESP-IDF Wi-Fi/FreeRTOS, native controller/coordinator host tests, pytest contract tests, Clang sanitizers, Ninja/Xtensa.

---

### Task 1: RED ownership transaction tests

**Files:**
- Modify: `tests/native/blufi_wifi_scan_retry_state_host_test.cc`
- Modify: `tests/native/blufi_wifi_scan_controller_host_test.cc`
- Modify: `tests/test_blufi_wifi_scan_contract.py`

- [ ] Add a threaded test where callback acquisition attempts to run between catch-path physical and logical commits and assert the shared submission mutex prevents it.
- [ ] Add deterministic outcome tests for exceptions before submit, after accepted driver return, after failed-start release, and after inline completion release; assert released leases remain Free and no retry tuple is published.
- [ ] Run `bash scripts/run_host_native_blufi_wifi_scan_retry_state_test.sh`, `bash scripts/run_host_native_blufi_wifi_scan_controller_test.sh`, and focused pytest; expect compile/contract failures for missing `StartOutcome` and shared commit helper.

### Task 2: Exact noexcept start outcome

**Files:**
- Modify: `main/boards/common/blufi.h`
- Modify: `main/boards/common/blufi.cpp`

- [ ] Define `OwnedWifiScanStartOutcome` with exact lease disposition (`kUnsubmitted`, `kSubmitted`, `kReleased`) plus accepted, consume-latched, drain/recovery, failure owner, and pending request data.
- [ ] Define one `CommitOwnedWifiScanStart()` helper that holds `wifi_scan_submission_mutex_` around `CommitSubmission()` and `CommitStart()` and returns the exact paired decisions without performing follow-up scheduling.
- [ ] Make `StartOwnedWifiScan()` noexcept and return `OwnedWifiScanStartOutcome`; catch internally and convert every boundary to an authoritative outcome before follow-up work.
- [ ] Update `TryStartOwnedWifiScanNow()` to consume the outcome and never infer ownership from the original local lease after start returns.
- [ ] Guard all post-release scheduling/completion/recovery/watchdog actions with local `try/catch` boundaries.
- [ ] Run focused native tests and pytest; expect all new tests to pass.

### Task 3: Full verification and commit

**Files:**
- Verify all files changed in Tasks 1-2.

- [ ] Run the nine-file focused pytest suite and confirm zero failures.
- [ ] Run all six native scripts and their ASan/UBSan/TSan variants.
- [ ] Run `ninja -C build esp-idf/esp-wifi-connect/libesp-wifi-connect.a` and the BluFi object target.
- [ ] Run `git diff --check`, request independent review, fix all Critical/Important findings, then commit the implementation.
