# Chat Runtime Timing Implementation Plan

> **For agentic workers:** Use subagent-driven-development for implementation and sequential spec/quality review. Root owns build, hardware and evidence.

**Goal:** Diagnose the first real-device transition to Idle without altering chat behavior.

**Architecture:** Fixed numeric recovery markers, nonblocking queue snapshots and scoped slow-work timing. No payload retention, extra worker or control-flow change.

**Tech Stack:** ESP-IDF5.5, C++17, cJSON, pytest, host ASan/UBSan/TSan.

Work in existing main checkout by explicit operator request. Preserve all dirty
changes; do not commit whole preexisting production files. Evidence directory:
`/Users/manhhodinh/Documents/TBOT/task-artifacts/chat-timing-ItEkEu`.

## Task 1: Diagnostic Instrumentation (Implementer)

Files: main/application.cc, main/application.h, main/chat_inbound_messages.h,
main/display/lcd_display.cc; optionally main/chat_runtime_timing.h for a small
scoped measurement helper. Tests: tests/test_chat_fault_diagnostics.py,
tests/test_chat_runtime_timing.py, tests/native/chat_runtime_timing_test.cc,
existing native fixture adaptations only where required.

- [x] Snapshot originals in the private evidence directory before edits.
- [x] Write RED tests for missing recovery markers and diagnostic timing.
  Require every RecoverChatStart/RecoverChatPlayout call site to identify its
  origin with fixed numeric code (defaults only for legacy test compatibility).
  Recovery logs must be emitted only on actual current recovery, not every poll.
  START receiver expiry logs distinguish expiry from identity cancellation.
- [x] Write native RED coverage for nonblocking queue snapshot API:

```cpp
struct Snapshot { bool available; size_t queued; size_t outstanding; };
// Empty -> {true,0,0}; admit4 -> {true,4,4}; take+retain1 -> {true,3,4};
// release1 -> {true,3,3}; held producer mutex -> {false,0,0}, without waiting.
```

- [x] Write timing RED coverage using controlled monotonic clock and captured
  log sink:49999us silent,50000us logs once; early return logs; exception unwind
  logs; backward clock never reports unsigned-wrap duration. Only numeric site,
  elapsed high/low words, no payload. Avoid logging while a display lock is held.
- [x] Run `python3 -m pytest tests/test_chat_runtime_timing.py
  tests/test_chat_fault_diagnostics.py -q`; save expected RED failure.
- [x] Implement smallest diagnostics. Take one try-lock snapshot on JSON failure
  before publishing fault; log availability, queue and outstanding counts.
  Use scoped timer at inbound dispatch, state handler and SetEmotion; threshold
  >=50000us. Recovery reason codes identify actual call site; document mapping.
  Preserve every predicate, short circuit, deadline, queue size and event order.
- [x] Run new tests plus `CHAT_APPLICATION_TSAN=1 python3 -m pytest
  tests/test_chat_json_admission.py tests/test_chat_playout_intake.py
  tests/test_chat_start_handoff.py tests/test_chat_runtime_timing.py
  tests/test_chat_fault_diagnostics.py -q`. Save complete logs. Self-review diff.

## Task 2: Verification (Root And Reviewers)

- [x] Spec reviewer compares actual scoped diff with approved spec and Task1.
- [x] Quality reviewer independently checks nonblocking semantics, RAII lifetime,
  logging privacy/volume, preserved callback behavior and executable proof.
- [x] Root runs full `python3 -m pytest tests -q`, target build with
  `IDF_PYTHON_ENV_PATH=/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env`
  and `/Users/manhhodinh/esp/esp-idf/export.sh`, then `idf.py build`.
- [x] Freeze source/binary hashes; copy exact candidate to private evidence.
  Verify Neweye asset hash unchanged. Record warnings and partition headroom.

## Task 3: Robot Trial (Root)

- [x] Confirm fresh attended safety clearance; never inject motion commands.
- [x] Ensure sole USB port is unowned; verify chip MAC14:c1:9f:d1:ac:20.
- [x] Read partition table and otadata without printing secrets; verify ota0 at
  0x20000 size0x3f0000, active sequence/state/CRC. Preserve full active app backup.
- [x] Flash only frozen app at0x20000 using esptool (no generic idf.py flash),
  separate verify_flash, then reset. NVS/assets/bootloader/metadata unchanged.
- [x] Open bounded600s private serial capture once, no command bytes; note that
  opening this port resets board. Ask operator for one Hi ESP after wake ready.
- [x] Correlate numeric firmware timeline and server window. Report earliest
  recovery and slow-work measurement, not unproven root cause. Preserve hardware
  FAIL until a later evidence-backed correction passes repeated acoustic E2E.
- [x] Update US-004 validation, TEST_MATRIX and artifact STATUS. No server
  deployment, key change, merge or broad commit in this diagnostic slice.

Result: hardware FAIL, first fault ping_delivery61216ms then recovery225.
Separate actual-method reproduction confirms an unsent Busy ping admission-ID
overtaking hazard; the real initiating contention is not yet proven. See
artifact ping-admission-diagnosis.md. Diagnostic plan complete; a bounded
behavior correction requires its own reviewed design. No readiness claim.
