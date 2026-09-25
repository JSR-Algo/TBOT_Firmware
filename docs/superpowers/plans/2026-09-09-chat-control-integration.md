# Chat Control Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Integrate actual normal-chat control and microphone event entry points with the existing workers before full callback activation and physical release.

**Architecture:** Application retains immutable ordered control intent and consumes one shared completion dispatcher. Worker tasks own socket and audio operations; the state handler alone authorizes listening. Cancellation follows the approved design's obsolete-control rule: never replay an old response's command against a replacement response.

**Tech Stack:** ESP-IDF 5.5, C++17, native barrier tests, pytest, ESP32-S3.

---

Continuation of approved Task4b, after the reviewed fifth slice. Work directly in
the existing firmware checkout; preserve dirty changes. No commits, new worktrees,
serial, deployment, flash or motor commands in this integration checkpoint.

## Ownership Contract

Same-owner accepted Stop/Abort/Wake controls remain FIFO through contention.
Source/response replacement explicitly supersedes obsolete records, preserving
active reservations until real retirement. Never rebind an old job to a new
generation. Sent means actual transport completion, not admission. A write already
submitted cannot be recalled; report that limitation, not fictitious cancellation.
Mandatory audio/transport cleanup remains independently retained through supersession.

Use four fixed intent slots including active records, with explicit fail-closed
recovery on capacity/allocation/identity exhaustion. This is app control bookkeeping,
not a larger outbound mailbox or PCM/Opus queue. Repeated Busy uses the same ID and
receipt deadline. No silent latest-value collapse or synchronous fallback.

## Task 1: Actual Control Entry And Fresh Listening

Files: modify `main/application.h`, `main/application.cc`,
`tests/native/chat_outbound_application_test.cc`,
`tests/native/chat_terminal_application_test.cc`,
`tests/test_chat_outbound_application.py`; add focused
`main/chat_control_intents.h` and `tests/native/chat_control_intent_test.cc` only
if fixed intent bookkeeping needs separation. Reuse existing worker/mailbox APIs.

- [x] Add failing runtime tests for actual selected SEND_AUDIO, Stop, Abort,
  StartListening, wake entry/continuation/direct invocation, listening watchdog
  and speaking timeout. Trap legacy socket sends, queue pops, decoder reset,
  processor toggles, wake reads and sound calls on Application.
- [x] Run `python3 -m pytest tests/test_chat_outbound_application.py -q` and
  record the expected behavioral failures before implementation.
- [x] Implement stable selected-normal-chat routing independent of an existing
  STOP/response. SEND_AUDIO only notifies the worker. Preserve lesson/legacy
  branches. Control entry immediately revokes obsolete capture/playback where
  appropriate, retains required cleanup and submits bounded owned intent.
- [x] Route completion once in the existing poll dispatcher: DrainAck,
  ListenStart and ordered control jobs retain separate exact correlations.
- [x] Extend state-owned rearm with Drain/User/Wake/Abort origins. Drain keeps
  original STOP+10s; explicit entry keeps its own receipt+10s, never synthetic
  STOP readiness. ListenStart requires prerequisite controls delivered; Arm
  additionally requires matching preparation/reset and current eligibility.
- [x] Keep pending visibly non-listening. Preserve existing state transition
  constraints, wake/watchdog thresholds, gesture cancellation and timestamps.
  No gesture starts merely because pending temporarily uses Speaking.
- [x] Add worker-owned wake read/cue obligations using the existing independent
  audio worker. Cue/wake requests cannot replace mandatory reset/preparation or
  reboot cleanup. Preserve wake/AEC policy; no app-side IsAfeWakeWord mutex.
  Do not call blocking PlaySound on that worker: a full decode queue gated by a
  newer reset would prevent the reset behind the cue from ever running. Add a
  narrow `AudioService::TryPlayChatCue` admission boundary in
  `main/audio/audio_service.h/.cc` if required; use the existing queue only,
  immutable cue/reset ownership and explicit Busy/failure. No partial-prefix
  replay; if incremental admission is required, retain a bounded static-asset
  cursor canceled on owner replacement. Test a held-full queue plus replacement
  reset: mandatory cleanup must progress before any remaining cue work.
  Selected implementation uses a <=64 KB immutable static Ogg view: count first,
  try-lock and require whole-cue capacity, then enqueue atomically with rollback
  on allocation/stale ownership. No incremental cursor or second packet backlog.
  Validate current cue assets fit this bound and report explicit admission failure.
- [x] Add and run behavioral cases with this required assertion sequence:

```text
hold transport before Stop -> request Stop -> request Wake
assert Application returned and capture is revoked
release transport -> assert wire order Stop then Wake then ListenStart
assert no Arm before preparation AND actual current ListenStart Sent
```

```text
accept old Abort -> hold transport admission -> accept replacement START
release admission -> assert old Abort is not replayed on successor
assert old reservation retires and cleanup remains recorded
```

```text
fill four accepted intent slots -> request fifth
assert visible recovery and no synchronous send or lost cleanup
advance clock beyond receipt+10s -> deliver stale Sent
assert no microphone authorization and no successor repaint
```

- [x] Cover both preparation/delivery orders, fresh manual and wake entry,
  exact deadline, same-ID Busy, source replacement, foreign Idle/lesson/passive,
  blocked decoder/send, and wake policy configurations 0/1.
- [x] Spec review and corrections, then independent quality review/corrections.
- [x] Root run full `python3 -m pytest tests -q`, focused three-TSan bundle,
  ESP-IDF build, binary hash and DWARF sizes. Record proof limits in US-004.

## Subsequent Activation Gates (Not Satisfied By Task 1)

Final Task1 local candidate accepted by scoped spec and quality re-reviews.
Root full1679/2 default opt-in skips108.60s; focused65 with all three TSan switches
54.81s; target0x397b30/9%free, SHA256
363c47334ea264c476d94a7764aacfbe37452ebc8c8a5d3c47f2f206bd88dcb2.
Application5320/Signals376/Cleanup88 bytes. All detailed evidence and limits in
US-004 validation. Actual selected fragments/helpers execute; full legacy bodies
are trapped. Combined app audio/display stubs and separate actual cleanup/Ogg
tests do not establish combined full-loop, target scheduling or acoustic proof.
Activation remains open; this final checkpoint supersedes rejected snapshots.

First frozen candidate is NOT accepted: full1679/2 opt-in skips and build
0x397870/9%free pass, but spec review found six correctness gaps. Fix/test old
terminal identity after local Abort, stamp-free owned Stop rendering, failed
FIFO retirement, explicit passive/Connecting wake, retained cue admission and
stopped-service publication race. Cue policy: retain accepted same-owner cue and
original deadline; return explicit Busy for another request instead of overwrite
or new queue. Stale owners may be explicitly superseded with result fencing.
Complete held ListenStart->Abort->fresh listen delivery proof. Then re-review.

Execution checkpoint (not reviewed/frozen): actual selected Stop regression first
failed on synchronous transport send; initial retained-control adapter cases then
passed. Selected entry, fresh-origin and cue/wake worker integration remain under
test. Root independent asset inspection: popup1148 bytes/9 Opus packets,
exclamation1663/15, vibration1574/14; each fits the existing120-slot queue at
60ms frames when empty. This does not prove target scheduling or audible output.

Full source JSON routing must preserve MCP, alerts, unpair/system, text/LLM/STT,
lesson forwarding and START gesture/watchdog behavior. Bound owned JSON/MCP
message count/lifetime without truncating payloads to the control-text limit.
Audit network/Wi-Fi/reboot/lifecycle and all shared-socket scheduled sends before
callback registration or drain capability advertisement. This next coherent
integration needs its own reviewed concrete implementation task after Task 1.

Matched firmware/server release requires exact candidate identity and runtime
closure plus server regression, API/WebSocket, physical and soak evidence. Only
then perform targeted app/assets flash with rollback and attended acoustic E2E.
The current dormant helpers and host tests do not establish production readiness.
