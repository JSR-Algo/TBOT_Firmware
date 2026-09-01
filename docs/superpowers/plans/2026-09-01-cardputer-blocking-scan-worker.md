# Cardputer Blocking Scan Worker Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep exact scan ownership fail closed and run Cardputer scan waits on one dedicated process-lifetime worker.

**Architecture:** Add explicit manager restoration outcomes and a host-testable worker request state. Integrate one FreeRTOS task with the board/UI so Application work only submits and applies short results.

**Tech Stack:** C++17, ESP-IDF Wi-Fi/FreeRTOS/Application scheduling, native host tests, pytest contracts, Xtensa build.

---

### Task 1: Fail-Closed External Scan Transaction

**Files:**
- Modify: `components/esp-wifi-connect/include/wifi_manager.h`
- Modify: `components/esp-wifi-connect/wifi_manager.cc`
- Test: `tests/native/wifi_manager_recovery_host_test.cc`

- [ ] Add failing tests for every rollback/finish driver failure and assert the exact manager token, lifecycle gate, and coordinator lease remain owned.
- [ ] Add an explicit external radio outcome and preserve snapshots until shared recovery proof.
- [ ] Route failed pre-submit ownership into draining recovery instead of abandoning the lease.
- [ ] Run manager native normal, ASan/UBSan, and TSan tests.

### Task 2: Host-Testable Worker Request State

**Files:**
- Create: `main/boards/m5stack-cardputer-adv/blocking_wifi_scan_worker_state.h`
- Modify: `tests/native/blocking_wifi_scan_lease_host_test.cc`

- [ ] Add failing tests for coalescing, exact revision completion, stale generation cancellation, and durable create/notify failures.
- [ ] Implement the minimal allocation-free state machine.
- [ ] Run Blocking UI native normal, ASan/UBSan, and TSan tests.

### Task 3: Dedicated Cardputer Scan Worker

**Files:**
- Modify: `main/boards/m5stack-cardputer-adv/wifi_config_ui.h`
- Modify: `main/boards/m5stack-cardputer-adv/wifi_config_ui.cc`
- Modify: `main/boards/m5stack-cardputer-adv/m5stack_cardputer_adv.cc`
- Modify: `tests/test_blufi_wifi_scan_contract.py`

- [ ] Add failing contracts proving manual and retry scans submit to the worker and never call the blocking body on Application task.
- [ ] Create one process-lifetime task and coalesced notification path.
- [ ] Move scan execution/result construction to the worker and schedule only short generation/revision-checked UI closures.
- [ ] Keep request durable across task creation/notification failure and cancel stale UI work.

### Task 4: Final Verification and Commit

**Files:** all files above.

- [ ] Run both native sanitizer runners.
- [ ] Run the full focused pytest gate.
- [ ] Compile exact Cardputer UI and board Xtensa objects.
- [ ] Run `git diff --check` and self-review the diff.
- [ ] Commit without amending; do not merge or flash.
