# Bounded Chat JSON Admission Recovery

Date: 2026-09-09. Parent: US-004 Google Live production audio gateway.
Status: approach approved by user; written specification awaiting user review.
Work directly in the existing firmware checkout and preserve unrelated changes.

## Evidence And Scope

The installed diagnostic application has SHA256
39923b44168c08e91c753d380c3ccca54c80ecdd74a9765b29e3c66c2aa700c8.
Two actual wake attempts in
`task-artifacts/wake-fairness-d8HjKG/diagnostic-wake-repro.log` reach
`chat_source_fault reason=json_admission` at 63676ms and 175896ms, before the
transport fault, Idle transition and SSL receive -76. Google produced audio;
these captures implicate firmware admission, not a proven provider outage.

The existing bool admission result conflates mutex contention, all four permits
being occupied, invalid input and allocation failure. The precise inner failure
on hardware remains unknown. Application consumes one item per coalescible event
without explicitly scheduling remaining backlog. These code-level hazards need
deterministic reproduction and separate diagnostics, not a claim that every
observed failure is queue exhaustion.

Goal: prevent transient handoff pressure from immediately tearing down a healthy
conversation, while preserving source isolation, ordering and bounded storage.
No server change or deployment is planned for this firmware-only correction.

## Selected Approach And Alternatives

Use explicit admission outcomes, bounded retry for transient pressure, and
bounded Application draining with reliable backlog notification. This narrowly
refines the earlier source-activation contract's immediate rejection policy.

Increasing capacity alone does not fix lock contention or notification loss.
Dropping or coalescing arbitrary JSON risks losing tools and user-visible state.
Neither alternative is included. No new worker or unbounded fallback queue.

## Admission Contract

`ChatInboundMessages` exposes distinct Accepted, Busy, Full, Invalid and NoMemory
outcomes for queued admission. Busy and Full guarantee no enqueue or ownership
transfer occurred; only those outcomes may be retried. Allocation/validation
failure is not retried. Existing callers needing a bool may use a thin wrapper.

Keep four shared outstanding permits, including queued messages, dispatched
contexts and retained asynchronous tool/lesson continuations. Popping a message
does not release its permit while another context owns it. No extra owned JSON
copy is retained while waiting for capacity. Borrowed callback JSON remains
valid only inside the callback and is copied once on successful admission.

For normal queued JSON, attempt admission only while its original connection,
protocol and connect-generation identity remain current. A transient failure
notifies Application, yields one scheduler tick and retries within a fixed
deadline: the earlier of receipt + 250ms and a nonzero existing receipt admission
deadline. Do not restart this budget on retry or after source adoption. Reject
overflow/invalid timing explicitly. The separate receipt + 10s dispatch deadline
is unchanged; it does not limit an already-started asynchronous tool's duration.

The 250ms budget is a maximum proposed admission wait, not a fixed delay on every
message or measured audio latency. Accepted messages return immediately. Source
replacement cancels the old request without faulting the successor. Permanent
failure or expiry faults only the still-current source; do not fake success or
silently drop a current message. Add fixed diagnostic outcome markers and numeric
attempt/wait information without payloads, transcripts, session IDs or secrets.

Keep the lesson inline TTS start/stop route and ordinary chat TTS start/stop
handoffs unchanged. Shared permit accounting must remain correct across these
routes. Preserve MCP reply ownership, cancellation and long-tool lifetimes.

## Consumer Progress And Concurrency

Application processes at most four queued items per poll, in FIFO order. It
releases the queue lock before validation or dispatch and checks each immutable
source and original deadline. Stale items are retired without dispatch, but do
not prevent later queued items from progressing.

The queue read result must distinguish Empty from Busy. Remaining backlog or a
contended read schedules another Application event; a genuinely empty queue
does not spin. Producer notification after publication and consumer rearming
must cover enqueue/drain races even when event bits coalesce. Each poll stays
bounded so audio, cancellation and lifecycle work retain scheduling opportunities.

The receiver currently holds the transport inbound gate during JSON callbacks.
Never wait on queue capacity while holding the queue mutex, dispatch under that
mutex, or make Application block acquiring the receiver gate. Audit every poll
and relevant dispatch path against this constraint before enabling retry. A
gate dependency or non-yielding retry discovered by tests blocks the candidate;
do not bypass it by increasing timeouts. Physical audio remains an acceptance
gate because bounded waiting can still delay subsequent frames on this socket.

## Preserved Behavior

Keep Hi ESP detection and wake-sync fairness, conversation state transitions,
listen/drain acknowledgments, qualified interruptions, echo suppression,
audio buffer sizes, task priorities, volume, Neweye assets and gesture policy.
Google credentials remain agent-config-only. No model or endpoint change.
This slice does not claim to fix remaining distortion or self-echo by itself.

## Verification And Release Gates

1. Before implementation, add native failing regressions using actual queue and
   extracted Application adapters: coalesced burst notifications, delayed
   consumer, lock contention and permit exhaustion. Distinguish the mechanism
   reproduced in software from the unresolved inner hardware condition.
2. Prove FIFO/exactly-once delivery, no fifth outstanding context, async permit
   retention/reclamation, and no lost event when enqueue races with drain.
3. Prove transient recovery before deadline, fixed expiry under sustained
   pressure, no allocation retry, stale-source cancellation and no late admit.
4. Exercise actual callback-to-Application flow with the receiver gate held;
   prove Application progress and bounded failure even when an outbound send,
   tool continuation or queue lock is stalled. No successor-source fault/replay.
5. Run relevant sanitizer-backed native tests, full firmware tests, target
   ESP-IDF build and independent review. Record skipped tests and limitations.
6. For the authorized flash continuation, revalidate the single target MAC
   14:c1:9f:d1:ac:20 and active OTA slot, preserve a rollback, flash only the
   selected app partition and verify its hash. Preserve NVS, OTA metadata,
   bootloader and Neweye assets. Do not reopen an active serial capture.
7. With the operator, capture at least three consecutive Hi ESP -> reply ->
   listening cycles without admission fault or server-unavailable exit. Record
   actual timing and pressure markers, then observe a roughly 30-second spoken
   response and silent interval for stutter/self-echo. Software PASS is not an
   acoustic PASS. Motion testing needs fresh safe-position/operator confirmation;
   no injected arm/head commands during unattended diagnosis.

Release remains unaccepted until the relevant real-device gates pass. If the
failure persists, use the distinct admission result to resume diagnosis; do not
stack buffer, microphone or server changes onto an unverified hypothesis.

## Workflow Checkpoint

- Project context, constraints and alternatives reviewed; no visual UI needed.
- User approved the proposed approach on 2026-09-09.
- Written spec self-review: scope, timing, ownership, failure semantics and
  hardware evidence requirements checked; no implementation performed.
- Next: user reviews this written spec, then writing-plans produces the test-first
  implementation plan. Build, flash and production acceptance remain pending.
