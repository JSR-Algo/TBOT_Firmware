# Unadmitted Connection Message Retry Design

## Status And Scope

Operator approved the narrow design in conversation on 2026-09-09.
Written specification review is pending before implementation planning.
This is a high-risk maintenance slice of US-004: existing robot conversation,
asynchronous transport ownership, and physical acceptance are involved.
Work directly in the existing main checkout as requested; preserve unrelated
dirty changes. Do not merge, clean worktrees, or commit existing production
changes as part of this specification.

Design checklist: context and constraints reviewed; alternatives presented;
narrow design approved; written spec and self-review completed; written review
pending; implementation planning follows written approval. No visual decisions.

## Evidence And Limits

Diagnostic app e54c6f28 on MAC14:c1:9f:d1:ac:20 detected wake at61116ms,
then reported ping_delivery and recovery225 at61216ms before returning Idle.
No JSON Full or START-expiry marker preceded that fault. The operator reported
no response after reconnect; no second wake was captured through591926ms.
The missed wake and audible stutter/echo remain unresolved acceptance gates.

An executable artifact using the actual Application methods and real outbound
worker/mailbox reproduces Busy ping1, newer Wake2/ListenStart3 admitted, retry
ping1 rejected Stale, logical Failed, ping_delivery, recovery225, Idle. It also
reproduces without wake data (ListenStart2). Four sanitizer/configuration
combinations passed reproduction checks with no clock advance, generation
change, or ping transport write. This proves a code defect, not the physical
trial's exact initiating contention; hardware admission IDs were not logged.

Evidence: workspace task-artifacts/chat-timing-ItEkEu/ping-admission-diagnosis.md
and repro_ping_admission.py. Relevant code is main/application.cc,
main/chat_connection_messages.h, main/chat_outbound_worker.h and
main/chat_outbound_mailbox.h.

## Alternatives

1. Selected: clear only an unadmitted connection-message physical attempt after
   SubmitChatOutbound returns Busy. Reconstruct it from the retained logical
   record on a later poll. Small blast radius; all connection-scoped FullText
   messages share the same safety rule, including ping and management replies.
2. Make request-ID assignment transactional in the common submitter. This
   affects Wake, ListenStart, Abort, DrainAck and their independent callers;
   wider review and regression surface are unnecessary for this defect.
3. Cancel or ignore failed ping around wake. Rejected: hides real transport
   failures and changes liveness semantics without repairing request ownership.

## Selected Behavior And Ownership

Change only the result handling at the end of PollChatConnectionMessages.
Its existing guard prevents submission when record.submitted is true. On Sent,
retain the admitted physical job, set submitted and retain the reservation as
today. On Busy, clear record.physical to its default value and return through
the existing poll path. Leave the logical record Pending. On Stale or Failed,
preserve existing failure behavior; do not treat arbitrary failures as retryable.

Busy at this boundary means the attempted job was not copied into the mailbox:
the worker delegates Submit directly to TrySubmit. A false return never enqueues
that attempted job. Additionally, this caller retries only an unsubmitted
record; successful admission always sets submitted, so a duplicate already-
admitted job cannot be reclassified as a new attempt by this change.

The next poll rebuilds kind, source, connect generation, shared payload and
deadline from the logical record. A zero request_id obtains a fresh monotonic
physical ID and current outbound generation through the unchanged submitter.
Never decrement counters, reuse IDs or bypass exhaustion handling.

Preserve logical ID, owner, payload, original absolute10s deadline and queue
position. Do not extend the deadline on Busy. Retain owner validation before
each attempt; an obsolete source cancels without sending on a replacement
connection. Retain the existing reservation retirement barrier before activating
a replacement worker. Do not reset admitted jobs, send again after a Sent or
Failed completion, or change existing proven-unsent completion/retirement rules.

No new loop, blocking wait, queue, worker, allocation or retry notification.
Existing completion/clock polling drives retries; repeated Busy remains bounded
by the original deadline. Control priority and queue capacities stay unchanged.

## Regression Proof

Use existing actual-method native fixtures, not a handwritten model of the
Application. Add failing desired-behavior cases before the production change.

- Hold the mailbox mutex, attempt ping, release it, process actual wake/control
  event ordering and then worker completion. Test wake-data0 and1. Require
  logical Pending after Busy, current source, no ping_delivery/recovery225,
  exactly one eventual FullText send and unchanged logical owner/deadline.
- Fill the mailbox, produce Busy, admit newer controls and retry after space is
  released. Verify no duplicate delivery and no premature logical failure.
- Repeat Busy across polls; preserve logical ID/payload/deadline. At the original
  deadline require failure with no new send, not another10s retry window.
- Retire the worker after Busy with the logical owner still current; retain
  the retirement barrier and allow a new physical attempt only after retirement.
- Replace the source after Busy; cancel the old logical request without sending
  its payload on the new source or faulting the new source.
- After successful admission, repeat polls before completion; physical identity
  remains stable and only one send occurs. Retained Sent/Failed completions must
  not trigger resubmission. Real send failure still faults a pending ping.
- Retain common submitter stale/duplicate/exhaustion tests and existing
  connection-message FIFO, management/unpair and stale completion tests.

Run new actual-method cases with ASan/UBSan and TSan, adjacent connection/
outbound/source/playout regressions, full firmware pytest suite, then ESP32-S3
target build. Record failed checks rather than weakening assertions.

## Hardware Trial And Release Boundary

Freeze reviewed source and binary hashes. Verify sole target MAC, active
partition metadata and recoverable app backup before app-only flash and separate
digest verification. Never use generic whole-device flash; preserve NVS,
bootloader, OTA metadata and Neweye assets. Opening serial resets this board;
check current capture ownership and open only one bounded private capture.

Keep agent-only API credentials, server image, model, volume, AEC, interruption
policy and gestures unchanged. No injected arm/head commands in this slice.
Confirm operator presence and clear travel before an attended voice trial if
the earlier confirmation is no longer current.

Test at least five separated Hi ESP starts, including one after a natural
reconnect if observed, followed by a short reply and another user turn. Record
wake detection, listening, server audio, audible output, return to listening and
operator observations. Do not induce a VPS outage solely to test reconnect.
If no reconnect occurs, leave that hardware gate pending. If wake fails, stop
calling the sequence a pass and investigate the distinct wake/audio recovery
path using captured evidence. Native recovery tests do not replace this gate.

The narrow defect is accepted only when regression proof passes and the real
wake sequence no longer hits its false ping failure. Overall production-ready
requires separate clean wake/relisten, stutter/distortion, echo, response-latency
and attended motion evidence. Do not infer acoustic success from queued audio
or infer real motion from command acknowledgement. Update US-004 validation,
TEST_MATRIX and artifact status with actual results.
