# Chat Recovery Continuation

Date: 2026-09-09. Parent: US-004 Google Live production audio gateway.
Status: direction and written specification approved by the user on 2026-09-09.
Lane: high-risk maintenance. Work directly in the existing firmware checkout,
preserving its dirty changes. Flash, server deployment and robot/Mac testing
are authorized within this repair; successful software tests alone do not
authorize a production-ready claim.

## Scope And Order

The approved repair sequence is (1) retained reconnect/wake intent, (2) bounded
presentation work outside the critical audio-control path, (3) interrupt/echo
verification, then matched release and physical acceptance. This specification
defines the first independently testable slice. Presentation and interrupt
changes must follow their own measured reproduction, not be bundled into the
recovery patch. Preserve Neweye, audio gain/volume ceiling, AEC, model, endpoint,
agent-only credentials, lesson behavior and gesture policy.

No new worker, transport, dependency, queue-capacity increase or admission-timeout
increase in this slice. No motor commands during unattended diagnosis.

## Evidence And Known Gaps

Current source application.cc SHA256:
7dd5e80c8c9ffcdb8270800e9afa5f1b7e03aa7eca077d1fa1a0b01f96d52716.
Current app SHA256:
25d005f09f054e9460300edfa7b5799957d19bfb0d5acdcbd188e48e6ec2d330.
Physical evidence: task-artifacts/robot-autonomous-Yv89Nw/RESULT.md.

The first capture has JSON admission Full, a scheduled reconnect, and a later
wake detector event without an Application transition or server wake. It has
no subsequent reconnect_tick/open success. SourceSelected is a sticky routing
flag; reconnect_count counts scheduling, not connection success.

Code hazards to reproduce independently:

- HandleReconnectTick returns while ProtocolWorkLifetime has pending cleanup,
  consuming the one-shot callback without retaining an automatic retry.
- HandleChatWake consumes a selected-route wake when TrySource fails, although
  the failed source cannot be reused and a worker reopen may still be needed.
- BeginChatListen also rejects an unavailable source; the explicit start-listen
  entry must not bypass or disagree with recovery ownership.

The exact physical branch that caused the observed silence remains unproven.
Recovery fixtures must execute the actual methods and lifecycle transitions;
counting calls to ScheduleReconnect alone does not establish recovery.

## Selected Approach And Alternatives

Retain one Application-owned recovery intent across temporary close/worker
retirement, then use the existing persistent open worker. Keep explicit user
wake/listen intent distinct from background reconnect; no old wire command is
rebound to a successor source. This gives bounded storage and retains existing
transport ownership rules.

Blindly retrying every pending lifetime action is rejected: reboot, reset,
unpair, Wi-Fi setup and intentional close must remain terminal for that intent.
Falling through to legacy synchronous audio/wake handlers is rejected because
it bypasses the selected chat route and can block Application. Increasing retry
frequency or bypassing the lifetime barrier is not a valid correction.

## Ownership And Lifecycle

Application owns at most one pending recovery intent. It records whether it is
background reconnect or an explicit wake/listen, its original time/deadline,
listening mode and the protocol/connect identity needed to reject stale work.
Use the existing 10-second control-intent budget for an explicit wake/listen;
coalesced duplicate events must not extend its original deadline. Background
reconnect keeps the existing capped backoff and slow retry behavior.

A transient fault close retains the intention to reconnect, but does not reopen
microphone capture. While cleanup or reservations are active, poll/notification
progress retains the intent without spinning, touching an owned protocol,
allocating additional requests or starting another open worker.

An explicit wake during recoverable close promotes background recovery to one
user intent. It must not reuse the old source or emit an old Wake/ListenStart.
After cleanup retires and a fresh connection is successfully adopted, consume
the intent once and construct fresh source-bound controls in the existing
Wake-before-ListenStart order. Read the wake text on the existing audio worker,
not synchronously on Application. Uplink arms only after existing preparation
and delivery gates pass. Recovery alone never authorizes cloud microphone input.

Intentional close, stop/cancel, unpair, lesson ownership, Wi-Fi provisioning,
reset, reinitialize or reboot cancels the retained intent. A late timer, worker
completion, source callback or audio preparation result cannot resurrect it.
Changing protocol identity invalidates the intent; only an explicitly tracked
recovery open can advance its connect identity. Failure of that open keeps
background recovery bounded by existing backoff; an expired explicit intent is
retired, never replayed later, and a new real wake is required to open user audio.

After background recovery without a live explicit intent, return to safe Idle
and rearm local wake detection through the existing audio cleanup mechanism.
Do not auto-resume a failed response or acknowledge incomplete playback.

## Diagnostics And Regression Proof

Add fixed numeric outcome markers for deferred/cancelled/expired recovery,
successful source adoption, and wake accepted/deferred/rejected. Do not log
payloads, transcripts, session identifiers, URLs or credentials. Preserve old
metric fields for compatibility but do not call their values connection success.

Test-first cases must cover:

1. Fault close still pending when the one-shot reconnect timer fires; cleanup
   then retires, exactly one worker open occurs, and a fresh source is adopted.
2. Wake before, during and after that cleanup; exactly one fresh wake/listen
   sequence, no send on the failed source, no capture before readiness.
3. Explicit start-listen versus wake, duplicate wakes, control FIFO pressure,
   original deadline expiry and clock boundary handling.
4. Intentional close, cancel, provisioning, lesson takeover, unpair, reset and
   reboot prevent later timers/completions from reopening conversation.
5. Old source events, mismatched generations, stalled worker retirement and
   allocation/open failure cannot mutate a successor or cause busy polling.
6. Idle wake rearm after successful background recovery and after explicit
   intent expiry; existing passive/lesson reconnect behavior stays intact.

Use actual Application methods and native lifecycle adapters with ASan/UBSan
and supported TSan. Run adjacent source activation, START, JSON, control/ping,
playout, lifecycle and lesson tests, then the full firmware suite and target
ESP-IDF build. Report skips and fixture limitations separately.

## Release And Physical Gates

Freeze source/config/toolchain/binary hashes and preserve a verified rollback.
Before app-only flash, verify sole ESP32-S3 MAC14:c1:9f:d1:ac:20 and active OTA
partition. Preserve NVS, OTA metadata, bootloader and Neweye assets; independently
verify the written app hash. Do not reopen a serial port already being monitored.

Test autonomous Mac-speaker wake with private serial/server/audio capture.
Exercise a recoverable robot-session fault only through a scoped existing
diagnostic mechanism; do not disrupt unrelated VPS clients. Require actual
source adoption and another wake/reply/relisten, not counters alone. Record
missing safe fault-injection access rather than inventing a PASS.

This recovery slice does not alone qualify stutter, distortion, self-echo or
motion. Parent acceptance still requires repeated full spoken turns, silence
intervals, qualified interruptions, acoustic quality and safe attended motion
checks. Deploy backend only for a tested server-side correction; do not redeploy
an unchanged healthy image as a purported fix. Stop trials at an unsafe hardware
condition or a repeat failure that requires a different root-cause investigation.

## Workflow Checkpoint

- Context, source identities, evidence and alternatives reviewed.
- User approved the three-stage direction ("duyet"). No visual design needed.
- Written self-review: ownership, cancellation, bounded retry and release gates
  checked; no queue/timeout or audio-policy change hidden in recovery.
- Written specification approved ("duyet"); execute the test-first plan.
