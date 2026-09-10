# Bounded Chat JSON Admission Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Recover transient inbound JSON pressure without disconnecting a healthy robot conversation.

**Architecture:** Retain the four-permit owned FIFO and immutable source/deadline contexts. Distinguish admission and dequeue outcomes, rearm bounded Application draining, and retry only transient receiver admission within the approved 250ms budget. Existing control/audio routing stays intact.

**Tech Stack:** ESP-IDF 5.5, C++17, cJSON, Python pytest, native ASan/UBSan/TSan.

---

Approved spec: `docs/superpowers/specs/2026-09-09-chat-json-admission-design.md`.
Direct existing checkout is explicitly requested by the user; no new worktree.
One implementer owns firmware/tests; root owns docs, independent audit, build and
hardware operations. Do not commit preexisting dirty source wholesale. No agent
may flash, open serial, deploy, or inject physical commands.

## Task 1: One Coherent Admission And Progress Correction

Status: implemented and frozen; independent spec and quality reviews PASS. Root baseline
source/playout/diagnostic suite: 32 passed in 21.52s, before candidate changes.
Implementer final targeted suite: 35 passed in 46.90s; new ASan/UBSan + TSan
suite: 6 passed in 16.49s. Root final full: 1714 passed, 2 optional skips in
231.45s; root native checks: 21 passed in 41.29s. All frozen hashes unchanged.

Files:
- Modify `main/chat_inbound_messages.h`: structured outcomes and four-permit FIFO.
- Modify only `MakeChatSourceCallbacks` and `PollChatInboundMessages` in
  `main/application.cc`, plus a declaration in `main/application.h` only if needed.
- Create `tests/test_chat_json_admission.py` and, if useful for readability,
  `tests/native/chat_json_admission_test.cc` for actual-method native regressions.
- Adapt existing native fixtures only for production API compatibility; preserve
  their original assertions and lesson routes.

- [x] Add a failing actual Application poll test using the existing method
  extractor and actual queue header. Enqueue four numbered messages, call poll
  once with one coalesced event, and assert FIFO delivery and zero outstanding
  contexts. Include a stale first message which must not prevent later progress.

```cpp
for (unsigned i = 0; i != 4; ++i) {
    auto* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "sequence", i);
    assert(app.chat_inbound_messages_.Admit(root, owner, 100, 0));
    cJSON_Delete(root);
}
app.PollChatInboundMessages();
assert((app.seen == std::vector<unsigned>{0, 1, 2, 3}));
assert(app.chat_inbound_messages_.Outstanding() == 0);
```

- [x] Run `python3 -m pytest tests/test_chat_json_admission.py -q`; record the
  expected assertion failure (only one dispatch), not a fixture compile error.
- [x] Add barrier tests for Busy vs Empty dequeue, Busy vs Full admission,
  NoMemory allocation hooks, invalid timing, async permit retention and slot
  reuse. Put hooks only in generated test copies or cJSON allocation hooks,
  never test-only methods in production. Run and record missing-behavior failures.
- [x] Implement structured queued admission outcomes with a compatibility bool
  wrapper and keep inline Own behavior unchanged. The intended public shape is:

```cpp
enum class Admission { Accepted, Busy, Full, Invalid, NoMemory };
enum class ReadStatus { Item, Empty, Busy };
struct ReadResult { ReadStatus status; ChatRequestContext context; };
// TryAdmit takes the same payload/owner/receipt/lesson/session inputs as Admit.
// TryTake returns one item without blocking; Pending returns true on contention.
```

  OwnLocked must check input, scan the same four weak permits, duplicate cJSON,
  assign immutable metadata, and only then publish the permit and queue entry.
  Busy/Full/Invalid/NoMemory must not consume capacity. Lock scope excludes
  dispatch and scheduler waits. Avoid a speculative copy before a free permit.
- [x] Change Application poll to the following bounded behavior, adapting names
  to the actual structured result without weakening its semantics:

```cpp
for (unsigned i = 0; i != 4; ++i) {
    auto read = chat_inbound_messages_.TryTake();
    if (read.status == ChatInboundMessages::ReadStatus::Busy) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
        return;
    }
    if (read.status == ChatInboundMessages::ReadStatus::Empty) return;
    auto context = std::move(read.context);
    if (!IsChatRequestCurrent(context)) continue;
    if (static_cast<uint64_t>(esp_timer_get_time()) >= context->deadline_us) {
        FailChatRequest(context);
        continue;
    }
    try { DispatchIncomingJson(context->root.get(), context->lesson_epoch, true, context); }
    catch (...) { FailChatRequest(context); }
}
if (chat_inbound_messages_.Pending())
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
```

- [x] Add failing actual receiver callback tests: recover Full after one permit
  is released, recover Busy after a lock holder yields, expire at the original
  deadline, reject already-expired receipt, no retry on NoMemory, and cancel on
  source/connect/protocol replacement without faulting the new source. Exercise
  retry with a real transport gate lease and an independently progressing
  Application consumer, plus a stalled outbound send and async continuation.
- [x] Implement queued JSON retry inside the existing callback (or a narrowly
  named extracted method if fixtures can exercise the actual adapter). Capture
  owner/session once; never rebind on retries. Compute deadline with checked
  receipt + 250000 and minimum nonzero receipt admission deadline. On each
  iteration: check owner/time, call TryAdmit, return on success, break on permanent
  failure, notify Application and `vTaskDelay(1)` only for Busy/Full. Preserve
  lesson inline and chat TTS start/stop early returns. Guard exact-source fault
  publication and include fixed reason plus numeric outcome, retries and elapsed
  time; no string payload logging. If allocation can cross the admission deadline,
  validate before publication and roll back uncommitted ownership rather than
  exposing late work. Raise any unavoidable gate dependency before activation.
- [x] Run `python3 -m pytest tests/test_chat_json_admission.py
  tests/test_chat_source_activation.py tests/test_chat_playout_intake.py
  tests/test_chat_fault_diagnostics.py -q`. All cases must pass; preserve old
  source, lesson, tool, timeout and ownership assertions.
- [x] Self-review and return exact files, red/green output, limitations and hashes.
  Root obtains spec review first, then quality review. Resolve findings before
  starting release verification. Scoped source commit is deferred because the
  checkout contains substantial preexisting integration work.

## Task 2: Root Verification And Evidence

- [x] Audit actual receiver gate and Application poll/dispatch dependencies;
  record whether all waits are nonblocking or independent of the held gate.
- [x] Independently run focused native tests with available TSan enabled and
  `python3 -m pytest tests -q`; capture full output and report optional skips.
- [x] Build with the existing IDF environment:

```sh
export IDF_PYTHON_ENV_PATH=/Users/manhhodinh/.espressif/python_env/idf5.5_py3.9_env
source /Users/manhhodinh/esp/esp-idf/export.sh
idf.py build
shasum -a 256 build/xiaozhi.bin
```

- [x] Update US-004 validation, TEST_MATRIX and this checklist with exact
  current-source results. No production-ready statement based on host tests.

## Task 3: Authorized Hardware Continuation

Status: user requested flash for testing; candidate7609e9cb app-only write and
separate verification completed.600s serial capture open; physical testing pending.
Built candidate7609e9cb preserved in
`task-artifacts/json-admission-EjoGMJ/xiaozhi-json-admission-7609e9cb.bin`.

- [x] Check connected port ownership without opening it. Revalidate target MAC,
  partition table and active OTA metadata before any write. Do not reset an
  active serial capture accidentally.
- [x] Capture selected application rollback. Flash only that app partition with
  esptool and separately verify_flash the same binary before reset. No generic
  idf.py flash; do not change NVS, otadata, bootloader, assets or server image.
- [ ] Keep one bounded serial capture open. Ask the operator for three Hi ESP
  cycles, then a roughly 30-second reply and silence. Correlate firmware admission
  markers with server reply timing and operator audio observation.
- [ ] Record pass/fail per wake, response, relisten, stutter and self-echo. Physical
  gestures remain separate until fresh safety confirmation. If the operator is
  unavailable, report the hardware gate open, not a successful end-to-end test.

## Plan Review

All approved spec sections map to Task 1 ownership/progress/retry, Task 2 software
proof or Task 3 physical proof. Existing behavior and server remain unchanged.
Independent implementation plus root gate audit can run concurrently; code and
spec/quality reviews are sequential. No competing source editors.
