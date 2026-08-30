# Course Mode HIL Diagnostic Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Task 7 HIL gate operate against a real build-gated firmware console handler and recover safely across serial disconnects and reboot.

**Architecture:** A bounded JSON diagnostic dispatcher owns validation and nonce-bound replies. An explicit Kconfig-only console adapter connects it to existing display, SD, audio, motion/rest, identity, and reboot seams. The Python collector sends the console command over an exclusive tty, reopens the stable port after reconnect/reboot, revalidates identity and capability, and fails closed when stop/rest cannot be confirmed.

**Tech Stack:** C++17, cJSON, ESP-IDF console, Python 3 PTY tests, pytest.

---

### Task 1: Diagnostic dispatcher

**Files:**
- Create: `main/course_mode_hil_diagnostic.h`
- Create: `main/course_mode_hil_diagnostic.cc`
- Create: `tests/native/course_mode_hil_diagnostic_test.cc`
- Create: `scripts/run_host_native_course_mode_hil_diagnostic_test.sh`

- [ ] Write native tests for bounded parsing, nonce echo, all required probes, safe motion limits, stop/rest, and reboot acknowledgement.
- [ ] Run the native test and confirm the missing dispatcher fails compilation.
- [ ] Implement the minimal callback-driven dispatcher and run the test green.

### Task 2: Build-gated physical console adapter

**Files:**
- Modify: `main/Kconfig.projbuild`
- Modify: `main/CMakeLists.txt`
- Modify: `main/main.cc`
- Modify: `main/application.h`
- Modify: `main/application.cc`

- [ ] Add an explicit disabled-by-default `TBOT_COURSE_MODE_HIL_DIAGNOSTICS` flag.
- [ ] Register/start the local console command only under that flag.
- [ ] Bind probes to running image identity, TFT scheduling, SD access, audio drain, bounded motion/rest, and delayed reboot.

### Task 3: Serial recovery and fail-closed cleanup

**Files:**
- Modify: `scripts/course_mode_hil_gate.py`
- Modify: `tests/test_course_mode_hil_gate.py`

- [ ] Add PTY tests that replace the symlinked tty after reconnect/reboot and require identity/capability revalidation.
- [ ] Add a PTY test where reopen fails and assert `safetyStopUnconfirmed` with a FAIL report.
- [ ] Run the focused tests red, implement stable-path reopen and cleanup, then run green.

### Task 4: Verification

- [ ] Run the native diagnostic test, HIL PTY suite, all-26 semantics suite where dependencies exist, and lesson coverage.
- [ ] Inspect the final diff and leave all changes uncommitted.
