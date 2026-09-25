# Unadmitted Connection Message Retry Implementation Plan

> **For agentic workers:** Use subagent-driven-development for the single software task and sequential spec/quality reviews. Root owns independent build, hardware and evidence.

**Goal:** Prevent an unadmitted Busy ping from becoming a false connection fault after wake/control priority overtaking.

**Architecture:** Clear only the unadmitted FullText physical attempt on caller-side Busy; preserve the logical record and all existing owner/deadline/duplicate protections. Common submitter, mailbox, worker and audio behavior remain unchanged.

**Tech Stack:** ESP-IDF5.5, C++17, pytest, ASan/UBSan/TSan, ESP32-S3.

Explicit operator request overrides isolated-worktree default: use existing main.
Preserve unrelated dirty edits; do not commit whole existing source files.
Evidence directory: /Users/manhhodinh/Documents/TBOT/task-artifacts/ping-retry-I43qvY.

## Task 1: Test-First Scoped Repair (Implementer)

Files: main/application.cc; create tests/test_chat_unsent_ping_retry.py and
tests/native/chat_unsent_ping_retry_cases.inc. Reuse run_terminal_application
from tests/test_chat_playout_intake.py, actual worker/mailbox implementations.

- [x] Snapshot application.cc in evidence before edits. Read approved spec.
- [x] Port artifact chat-timing-ItEkEu/repro_ping_admission.py into a desired-
  behavior regression using actual event-order fixtures, not a mock Application.
  Core assertion after held-mutex Busy is:

```cpp
auto* record = app.chat_connection_messages_.Front();
assert(record && !record->submitted);
assert(record->outcome == ChatConnectionMessages::Outcome::Pending);
assert(record->physical.request_id == 0);
```

  Drive actual HandleChatWake, PollChatOutboundEvents, HandleStateChangedEvent
  and worker RunOnce until delivery. Require exactly one FullText, no
  ping_delivery/recovery225, owner/deadline unchanged for wake-data0 and1.
  Include queue-full/newer-admission, repeated Busy/deadline, retirement barrier,
  source replacement cancellation, admitted-job duplicate prevention,
  retained Sent/Failed completion and real transport failure. Use existing
  fixtures and precise assertions for all approved spec regression bullets.
- [x] Run `CHAT_APPLICATION_TSAN=1 python3 -m pytest tests/test_chat_unsent_ping_retry.py -q`.
  Save RED showing desired behavior fails on unchanged source, not compile error.
- [x] Change only the result handling in PollChatConnectionMessages:

```cpp
    if (result == Result::Sent) {
        record->submitted = true;
        record->reservation = chat_outbound_reservation_;
    } else if (result == Result::Busy) {
        // No admission occurred; a newer control may overtake this attempt.
        record->physical = {};
    } else record->outcome = Outcome::Failed;
```

- [x] Re-run new tests then adjacent regressions:
  `CHAT_APPLICATION_TSAN=1 CHAT_OUTBOUND_TSAN=1 python3 -m pytest tests/test_chat_unsent_ping_retry.py tests/test_chat_connection_messages.py tests/test_chat_outbound_application.py tests/test_chat_outbound_mailbox.py tests/test_chat_outbound_worker.py tests/test_chat_playout_intake.py tests/test_chat_source_activation.py -q`.
- [x] Self-review scoped diff against snapshot; report RED/GREEN, file hashes,
  coverage and any residual gaps. No production commit, build, flash or network.

## Task 2: Independent Verification (Root/Reviewers)

- [x] Sequential spec review then quality review of scoped patch and tests;
  fix findings through implementer and re-review, without widening behavior.
- [x] Root independently runs new regression and full `python3 -m pytest tests -q`.
  Store full output privately; optional skips remain disclosed.
- [x] Build with IDF_PYTHON_ENV_PATH=/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env,
  source /Users/manhhodinh/esp/esp-idf/export.sh then `idf.py build`.
- [x] Freeze candidate app plus source hashes; verify assets hash unchanged,
  image checksum, actual partition headroom and build warnings. No merge/cleanup.

## Task 3: Attended App-Only Flash And Trial (Root)

- [ ] Check sole USB port, monitor ownership and current attended safety.
  Verify MAC14:c1:9f:d1:ac:20 before any flash write.
- [x] Read fresh partition table and otadata0xd000, verify active ota0 offset
  0x20000 size0x3f0000 and validated metadata CRC. Back up entire active app.
- [x] Write frozen candidate at verified active app offset only, esptool
  baud921600; separate verify_flash. Preserve NVS/assets/metadata/bootloader.
- [x] Reset then open one600s private serial capture with existing read-only
  capture_serial.py. Opening USB resets board; never open a second monitor.
- [ ] Ask operator for Hi ESP after wake-ready/source selected, short reply and
  follow-up. Repeat5starts; include natural reconnect if observed. No injected
  motion or induced production outage. Inspect logs using privacy-safe markers.
- [x] Record actual wake/latency/listen/audio/recovery results and user acoustic
  feedback in US-004 validation, TEST_MATRIX and artifact STATUS. Stop treating
  hardware as PASS at first failed gate. No production-ready claim without
  repeated physical wake/relisten, acoustic and separate motion evidence.

Current checkpoint: software tasks complete, final full1732PASS2optionalSKIP;
spec reviewPASS and quality reviewPASS. Target app25d005f0 ready. Task3 awaits
current operator presence/clear-travel reply before app-only flash and voice
test. Board remains in bootloader with a verified full-app rollback. No server
change, commit/merge/cleanup or production-ready claim.

Later operator explicitly requested flash and autonomous test. App25d005f0
flashed/verified; scoped acoustic stimulus after readiness produced wake,
listening, serveraudio/drain and relisten. E2E failed at recovery204 following
interrupt STOP, then JSON Full/reconnect. Five-start and motion gates remain
uncompleted; no injected motion. Mac speaker temporary mute restored. Acoustic
recording invalid for quality proof. See artifact STATUS and US-004 validation.
