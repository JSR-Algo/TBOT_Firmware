# Provisioning Lifecycle Concurrency Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make wake-word destruction, BluFi stack transitions, and successful provisioning rearm deterministic under concurrent task and callback execution.

**Architecture:** Workers publish task-local exit acknowledgements only after their final object access. A reusable transition gate serializes ESP-IDF BluFi operations without holding locks through SDK calls. One successful-teardown helper owns timeout cancellation, deinit, and conditional audio rearm.

**Tech Stack:** C++17, FreeRTOS event groups, `std::mutex`/`std::condition_variable`, ESP-IDF BluFi, Python source contracts, native pthread tests.

---

### Task 1: Worker final-access acknowledgement

**Files:**
- Modify: `main/audio/wake_words/afe_wake_word.cc`
- Modify: `main/audio/wake_words/afe_wake_word.h`
- Modify: `main/audio/wake_words/custom_wake_word.cc`
- Modify: `main/audio/wake_words/custom_wake_word.h`
- Modify: `tests/native/wake_word_lifecycle_gate_test.cc`
- Modify: `tests/test_wake_word_lifecycle_contract.py`

- [ ] Add a native failing scenario that blocks after final member access but before exit acknowledgement and proves reset still waits.
- [ ] Add source contracts requiring task-local handles, final-access-before-ack ordering, and no member/cv access after acknowledgement.
- [ ] Replace early atomics/cv publication with event-group exit bits and wait for those bits in `Shutdown()`.
- [ ] Run the focused native and source tests until green.

### Task 2: Serialized BluFi transitions

**Files:**
- Create: `main/boards/common/blufi_transition_gate.h`
- Modify: `main/boards/common/blufi.cpp`
- Modify: `main/boards/common/blufi.h`
- Create: `tests/native/blufi_transition_gate_test.cc`
- Create: `scripts/run_host_native_blufi_transition_gate_test.sh`
- Modify: `.github/workflows/build.yml`
- Modify: `tests/test_blufi_deinit_transaction.py`

- [ ] Add real multithread failing tests for shared same-operation result, conflicting-operation sequencing, reentrant failure, and exactly one owner callback.
- [ ] Implement the operation/owner/generation gate without holding its mutex while the owner executes SDK work.
- [ ] Wrap `init()` and `deinit()` in the gate and make transition-time BLE state fail closed.
- [ ] Add the native gate to the host-tests CI job and run focused tests until green.

### Task 3: Central successful teardown and idempotent rearm

**Files:**
- Modify: `main/audio/wake_word_lifecycle_controller.h`
- Modify: `main/audio/audio_service.cc`
- Modify: `main/audio/audio_service.h`
- Modify: `main/boards/common/blufi.cpp`
- Modify: `main/boards/common/blufi.h`
- Modify: `main/boards/common/wifi_board.cc`
- Modify: `main/application.cc`
- Modify: provisioning and claim Python contract tests

- [ ] Add failing native coverage proving rearm changes generation once only when provisioning-owned.
- [ ] Add source contracts enumerating every success helper call and every raw timeout/failure/manual teardown.
- [ ] Implement conditional audio rearm and `CompleteSuccessfulProvisioningTeardown(reason)`.
- [ ] Convert only approved success owners and leave non-success teardown raw.
- [ ] Run focused contracts until green.

### Task 4: Verification and commit

**Files:**
- Verify all modified files.

- [ ] Run focused Python and both native lifecycle/concurrency tests.
- [ ] Run the complete Python suite and record unrelated environment failures separately.
- [ ] Run lesson native coverage and the ESP-IDF production build.
- [ ] Run script syntax, CI contract, `git diff --check`, and final diff review.
- [ ] Commit without amending prior commits.
