# Chat Outbound Worker And Fail-Closed Recovery

Date: 2026-09-08
Status: written design approved by the user on 2026-09-08.
Parent: `2026-09-08-google-live-playout-echo-design.md`, US-004 high-risk maintenance.

## Evidence And Scope

Application sends microphone packets synchronously in MAIN_EVENT_SEND_AUDIO.
WebSocket and SSL sends take blocking mutexes; SSL starts its send deadline only
after acquiring its mutex. Moving only the drain ACK to a worker therefore leaves
Application vulnerable to a blocked microphone send. Listening state entry also
sends synchronously, and abort resets the decoder under potentially blocking
audio locks. Lifetime reservations prevent destruction races, not these stalls.

This extension covers normal conversational uplink, chat control delivery, and
the cleanup required by the existing drain controller. Preserve agent-only keys,
model selection, lesson/legacy behavior, qualified interruption, Neweye, gestures,
queue capacities and speaker ceiling. No serial access or motor actions during
implementation. Acoustic distortion still requires its separate measured repair.

## Alternatives

1. Dedicated persistent chat outbound worker (selected): isolate mic and chat
   control I/O from Application and the heartbeat worker. Costs a measured task
   stack and explicit lifecycle/control ownership.
2. Reuse the Open/Heartbeat worker: smaller task footprint, but heartbeat HTTP can
   monopolize the queue and delay chat interruption. Not selected.
3. Move ACK only or try-lock just WebSocket: smaller patch, but a mic send already
   waiting on SSL still blocks Application. Does not satisfy the recovery contract.

## Ownership And Data Flow

Application owns chat state, generations, drain controller, protocol publication,
and cleanup obligations. It publishes bounded work and receives fenced results;
it does not wait for chat network I/O, decoder reset, or transport destruction.

One persistent chat outbound task consumes the existing bounded audio send queue
and a bounded control mailbox. Notifications coalesce; no per-packet task creation
or second PCM/Opus backlog. A full control mailbox fails admission explicitly;
mandatory cleanup remains recorded independently and cannot be dropped. Stop,
abort and listen are ordered controls, not interchangeable latest-value flags.
Cancellation invalidates obsolete controls before selecting current work.

The existing Open/Heartbeat task remains separate with queue capacity two.
Every queued/running outbound job reserves the actual Protocol lifetime before
publication, captures its pointer and protocol/connection identity, and retires
ownership only after its final access. New admission stops before lifecycle
mutation. Application never destroys a protocol while a reservation is live.

Select control work before the next audio packet, without changing FreeRTOS
priorities. An in-progress transport write is not preempted or falsely reported
cancelled at the wire. All Application-reachable chat send paths, including wake,
listen, abort and error handling, must use the same admission boundary. Unrelated
traffic on the same socket must be audited for indirect Application blocking.

## Uplink Authorization

Tag each captured chat audio unit with its authorization generation before any
queue wait. Carry that identity through processing/encoding and packet delivery;
recheck before enqueue and immediately before transport submission. A boolean
checked only after rearm is insufficient: it would authorize old buffered speech.
Audio spanning an invalidation boundary must be discarded, not relabelled.

Cancel, disconnect, recovery and session replacement revoke the old generation
without waiting for queue locks. Worker-side cleanup discards obsolete packets.
Generation exhaustion fails closed rather than wrapping into an old identity.
Lesson/test audio ownership remains explicit and cannot become chat by accident.

Preserve the server's qualified interruption policy during ordinary playout;
this worker is not permission to mute the microphone throughout every response.
The server retains authoritative echo/tail filtering. A packet already submitted
to transport cannot be recalled; stale server-session/turn guards still apply.

## Drain And Recovery

At the original terminal stop, capture the coherent reset identity and playback
ownership without waiting. Busy capture fails closed; a later snapshot cannot
stand in for the original identity. The existing controller proves software and
hardware drain before proposing an ACK. Its original ten-second deadline includes
ACK admission and result delivery; retries do not extend it.

ACK results return to Application with request, protocol, connection and response
identity. Late or stale Sent never reopens input. On accepted current completion,
the state handler alone requests exactly one listen-start. Uplink authorization
opens only after current listen delivery and required audio readiness succeed.
No duplicate direct listen send from the drain-stop branch.

On timeout/cancel, Application immediately invalidates controller/outbound/input
ownership and exposes a non-listening recovery state. This path must not call a
generic state handler that blocks in ResetDecoder, queue drain, or network send.
Decoder reset and close/destruction execute as deferred cleanup, with a retained
obligation and nonblocking admission retries. No queue-full inline fallback.
Cleanup remains possible while the outbound worker is stuck in I/O: decoder work
must not be queued behind that send. Transport teardown waits for real ownership
to retire; a still-busy transport keeps recovery visible, not fake Listening.

The ten-second limit bounds entry into fail-closed recovery, not guaranteed
physical socket destruction. A partially submitted ACK cannot be retracted by
the current transport API. The server must reject expired/cancelled pending ACKs;
tests must cover late wire delivery as well as late local completion. Absolute
wire cancellation would require a separate transport API change and is not claimed.

## Approved Start Admission Refinement (2026-09-09)

The user approved a maximum 250 ms receiver-to-Application admission wait for
tts:start. This is a proposed bounded admission budget, not measured ESP32
scheduling or acoustic evidence. The receiver never waits for decoder reset or
playout readiness, and Application never waits on the receiver's inbound gate.
Application publishes a fresh response and pending reset identity before
acknowledging that exact immutable start request. Initial audio retains this
identity through the existing decode queue. Valid replacement starts remain
supported; the wire has no response ID that would justify rejecting them as
duplicates. Expiry prevents late admission and retains correctly scoped recovery.

Playback-only reset work uses the existing independent audio worker without
revoking qualified realtime interruption input. Old outbound ACK ownership stays
retained until actual retirement, even when the playback response is replaced.
No extra PCM/Opus backlog, buffer/priority tuning or callback-side reset writer.
The first implementation remains dormant: full callback routing, listen/rearm,
matched release and physical verification remain separate activation gates.

## Verification Gates

- Test-first native runtime barriers: hold a microphone send before terminal stop;
  advance the clock through ten seconds and prove Application enters recovery
  without releasing the send barrier. Repeat for ACK and listen-start sends.
- Hold decoder and queue locks: invalidation and recovery remain nonblocking;
  cleanup eventually runs after release, exactly once, without fake completion.
- Cancel during capture, processing, encode, enqueue, dequeue and send admission;
  old audio is dropped after rearm. Cover new turn, reconnect and identity limits.
- Test mailbox full, coalesced notifications, worker creation failure, heartbeat
  stall, pending teardown, failed/late result and replacement protocol. No lost
  cleanup, use-after-free, duplicate listen or unbounded pending allocation.
- Exercise actual Application adapters and packet flow, not only pure helpers or
  source-string contracts. Run sanitizers, existing lifecycle/audio/lesson/wake
  regressions and the ESP32-S3 build. Measure added stack/heap and worker latency;
  no speculative buffer or priority increases to make tests pass.
- Test matched server expiry/interrupt/ACK handling before capability enablement.
  Only a reviewed matched release may be deployed/flashed with rollback retained.
- Retain parent attended gates: ten replies plus silence, five interruptions,
  comparable applied65/92 audio, wake readiness, timing and user acoustic reports.
  Software success alone never establishes smoothness or production readiness.

## Self-Review

This is one firmware concurrency extension, not a transport rewrite. Local
recovery and eventual cleanup are distinct; no promise of retracting wire bytes.
Qualified interruption and lesson behavior remain release gates. The next step
after written review is a task-by-task implementation plan and RED/GREEN execution.
