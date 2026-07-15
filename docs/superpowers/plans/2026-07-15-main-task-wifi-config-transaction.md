# Main-Task WiFi Configuration Transaction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Settle realtime audio and protocol on the Application task before BLUFI initialization and roll back every failed entry safely.

**Architecture:** Entry callers enqueue an idempotent request. A fixed-state Application preparation ticket records recovery intent while synchronous main-task preparation, BLUFI setup, checked state publication, and exact-token rollback execute as one serialized transaction.

**Tech Stack:** C++17, ESP-IDF, pytest source contracts, native lifecycle harness.

---

### Task 1: Add failing transaction contracts

**Files:** `tests/test_wifi_board_provisioning.py`, `tests/test_realtime_voice_state.py`, `tests/native/wake_word_lifecycle_gate_test.cc`

- [ ] Require all entry paths to enqueue through one idempotent main-task request.
- [ ] Require preparation before reservation and BLUFI init, checked state publication, and exact cleanup before rollback.
- [ ] Model Listening, Speaking, Connecting, in-flight rejection, and publication-failure rollback in native coverage.
- [ ] Run focused tests and confirm RED.

### Task 2: Implement Application preparation and rollback

**Files:** `main/application.h`, `main/application.cc`

- [ ] Add a fixed-state preparation ticket and synchronous main-task preparation method.
- [ ] Reject in-flight/reset-pending work before mutation and settle realtime state without destroying `protocol_`.
- [ ] Add checked publication and rollback methods that resume prior realtime intent through Connecting.
- [ ] Run focused Application/state contracts.

### Task 3: Make WifiBoard entry one main-task transaction

**Files:** `main/boards/common/wifi_board.h`, `main/boards/common/wifi_board.cc`, `main/boards/common/blufi.h`, `main/boards/common/blufi.cpp`

- [ ] Add the atomic enqueue gate and route direct/timer entry calls through it.
- [ ] Execute prepare, reserve, Begin, Commit, init, checked publication, and visible mutation in order.
- [ ] Add exact-token abort teardown and invoke Application rollback only after cleanup succeeds.
- [ ] Preserve successful notification timing and non-BLUFI behavior.

### Task 4: Verify and commit

**Files:** all modified production, test, and contract files.

- [ ] Run focused Python/native gates, full pytest, exclusion suite, coverage, and ESP-IDF build.
- [ ] Run shell syntax and diff checks, inspect rollback paths, and create new commits without amending.
