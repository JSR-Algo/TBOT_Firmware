# Cardputer Deferred Wi-Fi Worker Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish process-lifetime worker handles safely and move all deferred Cardputer Wi-Fi connection work off the Application task.

**Architecture:** Add an atomic task-handle publication helper and a mutex-protected typed deferred-intent state. A dedicated connection worker executes blocking connection/reconnect work and durably hands immutable results back to Application.

**Tech Stack:** C++17 atomics/mutexes, FreeRTOS task notifications, ESP-IDF Wi-Fi manager, native pthread sanitizer tests, pytest source contracts, Xtensa build.

---

### Task 1: Race-Free Worker Handle Publication

**Files:**
- Create: `main/boards/m5stack-cardputer-adv/process_lifetime_worker_handle.h`
- Modify: `main/boards/m5stack-cardputer-adv/m5stack_cardputer_adv.cc`
- Modify: `tests/native/blocking_wifi_scan_lease_host_test.cc`

- [ ] Add a two-thread test where notification probes run before and during handle publication and eventually observe exactly the published handle.
- [ ] Run the native runner and verify the new test fails before implementation.
- [ ] Implement release-store/acquire-load publication and use it for every scan/connection task notification.
- [ ] Run normal, ASan/UBSan, and TSan variants.

### Task 2: Typed Deferred Intent State

**Files:**
- Create: `main/boards/m5stack-cardputer-adv/cardputer_wifi_deferred_intent_state.h`
- Modify: `tests/native/blocking_wifi_scan_lease_host_test.cc`

- [ ] Add failing tests for concurrent credentials/reconnect publication, coalescing, exact revisions, stale generation cancellation, notification failure, durable immutable results, and exactly-once completion.
- [ ] Implement the minimal mutex-protected state machine with pending, in-flight, and result-ready phases.
- [ ] Run normal, ASan/UBSan, and TSan variants.

### Task 3: Dedicated Connection Worker

**Files:**
- Modify: `main/boards/m5stack-cardputer-adv/m5stack_cardputer_adv.cc`
- Modify: `tests/test_blufi_wifi_scan_contract.py`

- [ ] Add failing contracts proving `AttemptWifiConnection`, deferred reconnect polling, and the Application closure contain no bounded connection wait or direct `TryWifiConnect()` execution.
- [ ] Create one process-lifetime connection task and durable notification path.
- [ ] Publish credential/reconnect intents from UI paths; notify only after external scan ownership clears.
- [ ] Execute SSID persistence, lifecycle transitions, connection wait, and reconnect on the worker without holding UI/deferred mutexes.
- [ ] Store credential results before scheduling a short generation-checked UI closure; retry scheduling without repeating connection work.
- [ ] Add a deterministic host model proving an Application sentinel runs while the connection worker remains in a modeled 10-second wait.

### Task 4: Verification and Commit

**Files:** all files above.

- [ ] Run both native sanitizer runners.
- [ ] Run the full focused pytest gate.
- [ ] Compile exact Cardputer UI and board objects.
- [ ] Run `git diff --check` and review concurrency/ownership paths.
- [ ] Commit without amending; do not merge or flash.
