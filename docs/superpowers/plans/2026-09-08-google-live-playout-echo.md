# Google Live Playout And Echo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent premature microphone reopening and identify/repair measured distortion and playback stalls, with attended robot evidence.

**Architecture:** Use negotiated, connection/response-scoped playout acknowledgement. Firmware determines completion without blocking its main task; the server preserves its existing echo guard until completion and residual tail. Treat clipping and scheduling as separately measured causes, not consequences of volume alone.

**Tech Stack:** ESP-IDF C++17, FreeRTOS, ES8311/I2S, Python3.10 asyncio, Google Live, pytest and native sanitizer tests.

---

## Working roots and constraints

Firmware root F: `/Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware`.
Server root S: `/Users/manhhodinh/Documents/TBOT/robot/esp32-server/.worktrees/agent-only-gemini-key/main/tbot-server`.
Paths below are relative to F or S as explicitly marked. Use current firmware
checkout as requested; preserve existing uncommitted audio and display work.
Do not stage unrelated changes. No motor commands, API/model changes, NVS erase,
speaker-cap removal or buffer enlargement. Read each root's applicable AGENTS.md.

Spec: `docs/superpowers/specs/2026-09-08-google-live-playout-echo-design.md`.
The spec's10s recovery deadline must never be interpreted as successful drain.

## Task 1: Capture reproducible baseline and effective gain

Files: F `main/audio/codecs/es8311_audio_codec.cc`,
F `main/audio/audio_service.cc`, S `core/voice/google_live/audio_bridge.py`.

- [x] Run existing focused firmware baseline:
  `python3 -m pytest tests/test_audio_output_timing.py tests/test_audio_playback_refill.py tests/test_tts_drain_ack_contract.py tests/test_realtime_voice_state.py -q`.
  Save counts and separate pre-existing failures from introduced failures.
  2026-09-08 result:175 passed/1 failed; prior diagnostic %lld format violates
  target contract. Diagnostic implementer assigned correction before integration.
- [ ] Read effective config merge and provider construction; inspect only numeric
  output_gain, never dump agent configuration or credentials. Base1.35 is not
  proof of the effective connected client's gain.
- [ ] Capture a30s reply followed by15s silence, preserving applied volume and
  current GIF. Record timing, drain timeouts and user-observed crackle. Do not
  generate motor actions or mistake a synthetic API test for physical E2E.
  Attempted45s capture remained wake-idle; not an acoustic baseline pass.
- [x] Separate driver-call elapsed time from surrounding log cost using paired
  monotonic timestamps, bounded maxima and counters. No per-sample logs. Add
  a native test before any new accumulator logic; test initial state, rollover
  between windows, gaps between turns and duration aggregation.
  Implemented diagnostic split; independent spec and quality reviews passed.
  Root rerun:178/178 focused tests pass, including prior format regression.
  Hardware measurements with this split remain pending matched-candidate flash.

## Task 2: Firmware completion predicate and native state machine

Execution split:2a decode-in-flight/reset-epoch fence implemented and passed
independent spec/quality reviews. Root focused rerun197 passed.2b pure drain
state machine passed independent spec/quality reviews, including terminal-token
replay regression. Root helper/audio focused rerun181 passed. Codec-tail proof
now passed independent spec/quality reviews. Root fresh204 focused tests and full
ESP32-S3 build pass; app0x38b1b0,10%free, SHA256
ae4e589b1692f55488305a24e4c52459da75504c3ee66ca82732ded423ccfe98.
No flash. The codec certifies exact digital submission and N+1 TX EOF progress,
not analog audibility or every masked intermediate managed lifecycle return.
Actual target disassembly regression checks sole-ISR-writer atomic load/store
in IRAM with no calls/branches/loops; general lockfree-RMW assertion was false
on this toolchain and was replaced with verified operation-specific evidence.
Independent read/write locks preserve duplex; lifecycle owns both. Open failure
keeps prior fail-fast behavior after review correction. AudioService snapshot
and application handshake integration remain pending.

Create F `main/audio/conversation_playout_drain.h` and
F `tests/native/conversation_playout_drain_test.cc`; modify F
`main/audio/audio_service.h`, `main/audio/audio_service.cc` and
`tests/test_tts_drain_ack_contract.py`.

- [x] Write failing native tests for the following transitions, using explicit
  monotonic times rather than sleeps:

```text
start A at0; queue empty + decode-in-flight => Pending
A decode done + output-in-flight => Pending
A output done + DMA tail outstanding => Pending
A all stages drained => Complete once
start B; ack/completion for A => Ignored
cancel B => Cancelled, never Complete
start C at0; poll at10000000us with pending work => TimedOut once
```

- [x] Run the test with clang++ C++17 and ASan/UBSan; confirm missing state-machine
  behavior fails before implementing it. Keep state owned by the app task.
- [x] Implement states Idle/Pending/Complete/Cancelled/TimedOut with a captured
  connection epoch and response generation. `Poll(now, snapshot)` returns an
  action once and never waits. Reject empty/oversized IDs at the message boundary.
- [x] Track decode-in-flight under the existing queue mutex, set before popping
  the last packet and clear on success, failure and stale-generation handling.
  Include it in the playback-completion predicate. Ensure old decoded PCM cannot
  be enqueued after cancellation. Add a runtime race regression with barriers.
- [x] Account for codec DMA tail after the final successful write using actual
  DMA geometry and sample rate, or a driver completion primitive. Validate the
  chosen mechanism against ESP-IDF ownership semantics before enabling it.
  Codec implementation reviewed/built; capability not advertised yet. Attended
  analog-tail proof remains required in Task6.
  Do not assume `esp_codec_dev_write` means audio has left the speaker.
- [ ] Run native tests plus existing drain/worker lifecycle regressions; review
  queue locking and generation handling before proceeding.

## Task 3: Nonblocking firmware chat drain integration

Integration audit notes: existing OnIncomingJson supplies callback_transport_epoch;
fence scheduled delivery against the current transport and protocol lifetime,
not only protocol object replacement (one object reconnects). Normal stop currently
increments speaking_generation inside Schedule; the new chat path must capture
the actual playback generation without invalidating queued tail PCM. New start,
interrupt and disconnect must cancel pending state before stale work can publish.
Only advertise capability when the codec supplies supported completion evidence.
Snapshot readers and session rearm must not block behind a driver write.

Detailed audit correction: callback_transport_epoch is a lesson-abandonment epoch,
not a guaranteed per-reconnect identity. Preserve it; add a separate atomic healthy
connection epoch publication to ConnectionInboundGate (zero on FailCurrent too).
Conditional ack must hold a nonblocking matching gate lease across send to avoid
reconnect between identity check and send. Busy defers; stale cancels; failed send
fails closed. Track delivery after helper completion so Busy cannot lose the ack.
Snapshot uses queue try-lock without decoder lock. Capture software reset/generation
at original stop time, but latch hardware epoch only once queues and flights are
quiescent, excluding legitimate delayed EnableOutput on short replies. Never
relatch a changed hardware epoch. Keep realtime input processing unchanged pending.
Explicit post-stop intake fence is needed: existing OnIncomingAudio accepts state
Speaking even when tts_audio_accepting=false. Drain timeout must not invoke generic
AbortSpeaking/HandleSpeakingTimeout (both reopen Listening), nor block inside
ResetDecoder. Defer decoder reset/queue clearing to a worker or use a nonblocking
cancel request. Exactly one state-handler listen frame after successful ack;
do not repeat direct SendStartListening/EnableVoiceProcessing from the stop branch.

Execution slices (sequential implementation, spec then quality review):
- [x] 3a transport prerequisite: publish healthy connection identity atomically;
  add nonblocking matching lease and conditional chat acknowledgement result
  (Busy/Stale/Sent/Failed). Preserve lesson epochs and legacy ack API. Native
  barrier tests must prove lease serialization, stale rejection and failure
  publication. Do not advertise capability or change application behavior yet.
- [ ] 3b audio snapshot/controller prerequisite: coherent nonblocking snapshot,
  late hardware epoch latch, original stop deadline and deferred cancellation.
  Test queue/driver busy, generation/reset changes, terminal ack delivery retry
  and fail-closed transitions using actual helpers and injected effects.
- [ ] 3c application integration: connect prerequisites to chat stop/start,
  app-event polling, state/lifecycle fencing and supported capability. Preserve
  legacy/lesson behavior and qualified user interruption; target build required.

Transport inspection: matching gate try-lock does not make socket Send nonblocking.
WebSocket::Send takes send_mutex_, then EspSsl::Send takes another mutex and may
wait for its transport deadline. Therefore conditional acknowledgement sending
must execute off the Application poll task with protocol lifetime protection;
only its fenced result returns to the app task. Keep this distinct from Busy
gate acquisition. Do not describe synchronous TrySend as nonblocking end-to-end.

3a checkpoint: initial implementation passed248 combined firmware tests and full
target build (app0x38b450,10%free; SHA256
f3dbfcc8c4725d770339dbd87e51c9064677ccc723de2a371965295da7dd7ebb).
Build includes pre-existing unused LessonCourseDeliveryApplied warning. No flash.
Spec review then found current OnDisconnected failed to clear healthy identity;
correction is pending. Also investigating actual socket publication under the
matching lease: epoch publication precedes handshake, while socket ownership
assignment currently occurs later. Pure gate tests alone cannot certify this
pointer/epoch binding. Do not approve3a until these boundary checks are resolved.
Confirmed socket-binding gap: new connection epoch becomes healthy before the
replacement socket is installed. Approved bounded correction uses an installed
socket epoch checked under the matching lease (opening returns Busy, no send),
publication/retirement under lease with destruction outside it, and scoped detach
that cannot remove a newer socket. Legacy application flow/capability remain
unchanged. Require runtime evidence at actual publication/disconnect boundaries.
Follow-up: disconnect, installed socket binding, scoped retirement/notification
and3 stale handshake failure paths corrected with RED/GREEN regressions. Spec
re-review approved3a. Root fresh251 combined firmware tests passed in9.66s;
target build passed (app0x38b5d0,10%free; SHA256
3b99162ba283ed03b8caf560737380f2ba7f364cbffebfc6fb0f9dc102241afa).
No flash. Quality review approved; ordinary legacy SendAudio concurrency is not
claimed repaired by this conditional-ack prerequisite.
3b execution starts with a bounded read-only AudioService snapshot slice (3b1),
before controller/deferred reset and application effects. Queue try-lock failure
must leave the caller's output unchanged; snapshot reports actual queues/flights,
reset/generation and hardware state without inventing successful drain.
3b1 passed spec/quality review. Fresh root252 combined tests passed in15.39s;
target build passed app0x38b5c0/10%free, SHA256
d80704aff9d505b32fcfda0e935aa5e7328bf899f1b375f05e1110a97fdb5c6f.
No flash. Original-stop reset identity capture, late hardware epoch latch,
ack worker delivery tracking and deferred timeout cleanup are still pending.

App integration audit for3c: existing network worker serializes OpenChannel and
Heartbeat through a2-item static queue. connect_in_flight_ protects protocol_
lifetime only for existing connect operations; merely enqueueing an ack pointer
does not inherit that protection. Future ack dispatch must reserve/track protocol
use through queued and running work, defer reset/reboot/intentional close safely,
and fence app result delivery by protocol+connection+response. Queue-full or
deadline exhaustion must fail closed, without increasing queue capacity. Do not
call blocking CloseAudioChannelByIntent/ResetDecoder inline from the new poll's
timeout branch; split immediate input/intent fencing from worker cleanup.

3b2 controller checkpoint: new ConversationPlayoutController composes the reviewed
drain helper and owns deferred effects. Original stop deadline includes ACK
delivery; Busy observation/delivery never renews it. Hardware epoch latches only
after stable software quiescence, and Sent requires fresh ownership/audio proof.
Independent spec and quality reviews found no actionable issues. Root fresh
controller/audio/transport/gesture/realtime/lesson regression:219 passed in11.98s.
No Application wiring or capability at this checkpoint;3c integration is underway.
Fresh server focused regression:392 passed in34.81s on host Python3.14.6 with one
SDK deprecation warning. This is not production-Python or release-bound evidence;
local Docker daemon is unavailable, so no fresh production-image run is claimed.

3c audit pause (no integration code edited): actual protocol lifetime cannot use
connect_in_flight_; HandleConnectWatchdog clears it while Open work may still run.
InitializeProtocol also runs from ActivationTask, so an app-owned reservation
counter alone races replacement. Reset/reprovision/activation/reboot and direct
close paths must share real queued/running worker ownership. Proposed prerequisite
is app-task serialized protocol publication/replacement with deferred activation
completion and a separate worker lifetime gate, preserving queue capacity2.
User approved this activation/lifecycle ownership prerequisite in the next turn.
Execution is limited to real worker reservations and serialized protocol
publication/replacement with preserved activation completion ordering. Separate
RED/GREEN runtime tests and spec/quality reviews precede drain integration.
Software reset identity also cannot be read through an unlocked accessor:
AudioDecodeFence::ResetEpoch is raw uint64_t guarded by audio_queue_mutex_, not
an atomic. Original-stop identity needs coherent nonblocking publication or a
reviewed fail-closed busy-stop policy, not a later snapshot presented as original.
No capability advertisement, matched release, flash or acoustic pass at this pause.

Approved lifecycle prerequisite completed locally: ProtocolWorkLifetime retains
queued/running ownership independently of watchdog status. Protocol publication
is Application-owned; reset/reinit/reboot/close wait for real reservations, and
activation/claim completion follows publication/startup. MQTT Start stays on the
existing worker with queue capacity2 unchanged. Quality review caught and fixed
watchdog status stranding, skipped MQTT startup premature activation, and lost
reconnect retry while reservations remain busy. Native adapters execute actual
worker, watchdog, reconnect/admission, close/reset/reboot and claim completion
paths with barriers and ASan/UBSan. Full InitializeProtocol registration remains
source-reviewed, not fully executed by host stubs. Spec review and final quality
re-review found no remaining actionable lifecycle findings.
Root fresh402 combined firmware tests pass in18.21s. Final ESP32-S3 build passes,
app0x38dfd0/10%free, SHA256
cdbad751e8a333233549d1cb8d4d760826cc61116f5beaa10d77ec23be5ba908.
No flash or capability.3c chat wiring/ACK worker and original-stop reset identity
remain pending; this prerequisite does not repair acoustic acceptance by itself.

Original-stop identity execution: add single-writer split32 reset epoch publication
under the existing queue mutex, with bounded sequence-checked atomic load/store
reads and unchanged output on Busy. Mark publication busy before incrementing the
raw fence and publish before waiting for the decoder lock. Do not read the raw64
epoch unlocked or assume target atomic64 operations are nonblocking. This is a
read-only ownership accessor prerequisite, not capability or ACK integration.

Further3c audit: MAIN_EVENT_SEND_AUDIO still uses synchronous SendAudio on the
app task; it shares WebSocket/SSL send mutexes with future ACK worker sends.
Do not claim that moving ACK alone makes the app deadline enforceable while
another app send can wait behind it. Integration must explicitly account for
that contention without disabling qualified realtime interruption throughout
playout. ACK admission occurs only after output quiescence. Timeout's immediate
input/intent fence must also avoid state-handler audio reset side effects until
deferred cleanup is safe. No extra speculative audio tuning is authorized by this
audit; scoped runtime effect tests must prove the chosen integration behavior.

Modify F `main/application.h`, `main/application.cc`,
`main/protocols/websocket_protocol.cc`; tests F
`tests/test_realtime_voice_state.py`, `tests/test_tts_drain_ack_contract.py`.

- [ ] Write failing tests proving chat stop cannot execute the existing
  `action=continue_listening` timeout fallback; lesson behavior stays isolated.
- [ ] Add hello capability `conversationAudioDrainAck` only with the complete
  implementation. Chat drain IDs use a distinct `chat:` namespace.
- [ ] Route supported chat stops into the state machine instead of the blocking
 2s wait. Poll via a lightweight periodic app event; timer callback only posts
  work. Capture socket epoch/generation before scheduling and recheck on delivery.
- [ ] On Complete send existing `SendTtsDrainAck(id)` exactly once. Keep terminal
  server echo suppression authoritative; do not restart mic from stale callbacks.
- [ ] On timeout explicitly cancel/abort output and fail closed. If quiescence
  cannot be established, close the conversation with a recoverable error. Test
  that timeout does not report successful drain or leave a permanent fake-listen
  state. Normal completion must not flush the final syllable.
- [ ] On explicit interrupt/new start/disconnect invalidate old pending state.
  Preserve the existing qualified interruption path and lesson ownership.
- [ ] Run full realtime-state and drain tests; then build F with the existing
  ESP-IDF environment. Do not flash until server compatibility tests pass.

## Task 4: Server handshake, guard ownership and compatibility

Review checkpoint: initial22new+138existing host tests pass; exact production
Python3.10 new22 pass. Spec review nevertheless reproduced two P1 gaps: pending
mic branch bypassed positive echo/wake suppression under optional policy, and
watchdog recovery/receive cleanup could mutually cancel/await (RecursionError).
Both corrected with regression tests. Independent spec re-review reproduced the
original failures and now passes: suppression parity and concurrent cleanup are
restored. Fresh host25new+138existing=163 passed (one SDK deprecation warning);
isolated exact-production Python3.10 new25 passed with fixture diagnostics.
Quality review found and confirmed correction of a P2: qualified interruption
during pending drain bypassed runtime-error recovery. Added actual client interrupt
OSError regression, preserving CancelledError propagation. Independent spec delta
review passes26 tests. Fresh host and isolated exact-production Python3.10 both
pass164 combined tests; corrected fixture teardown eliminates pending-task and
never-awaited markers. Dependency/fake-connection diagnostics remain. Quality
review approved with no remaining findings. Task4 local implementation complete;
matched firmware/release/acoustic gates remain open. No deployment.
Expanded host regression across the14 existing approved test files plus new chat
and lesson drain suites:845 passed in43.60s (one SDK deprecation warning).
This direct test invocation is not Git-bound release evidence. Current pinned
release closure/node inventory does not include the new conversation module/test;
reviewed inventory regeneration and exact candidate binding remain required.
Expanded Python3.10 verification: first unittest invocation had3 import errors
because production image has no pytest. Disposable container with pytest8.4.2 /
pytest-asyncio1.2.0 installed only into temporary test tooling then passed all845
in53.79s,3 dependency deprecation warnings; no pending-task markers. Source was
read-only and production runtime packages/server were not modified. This is not
the pinned release runner and does not replace its gates.

Read-only audit: audio_end is delivered to bridge BEFORE provider despite the
hook docstring. Do not await device ack in the bridge receive path; a provider-owned
watchdog keeps pending identity/guard after the bridge finalizes. Register after
delivery receipts settle, before terminal stop. Defer provider post-reply hold and
bridge echo-tail anchor until accepted ack. Pending guard must outlive clearing
google_live_audio_out_started_at. Do not represent device drain as WAITING_MODEL:
its frame-driven release path precedes echo qualification and can swallow real
interruptions. Preserve lesson ID priority and synchronous control ack dispatch.
Cancel pending ownership on resource close, clean response advance and interrupt;
watchdog cleanup must never await itself. Existing make_finalizing_bridge fixtures
and RobotOutputEchoGateTest provide integration regression entry points.

Create S `core/voice/google_live/conversation_playout.py` and
S `tests/test_google_live_conversation_playout.py`; modify S
`core/voice/google_live/audio_bridge.py`,
`core/voice/session_provider/google_live.py`,
`core/handle/textHandler/lessonMessageHandler.py`.

- [x] Write asyncio tests with controlled events and a fake clock for ack-before-
  waiter, duplicate/stale ack, lost ack, new response, reconnect, cancellation,
  lesson/chat separation and a legacy peer without capability.
- [x] Implement one pending connection/response-scoped drain object. Register
  identity and waiter before sending stop, then attach `drainId` only when the
  peer advertises `conversationAudioDrainAck`. Use unique chat-prefixed IDs.
- [x] Route received chat acks to the active provider only; retain the existing
  lesson ack route. Return false for unknown IDs without releasing any guard.
- [x] Keep output/echo guard owned by the pending drain until matching ack;
  start the residual echo tail after ack, not terminal network send. Samples
  captured under suppression are discarded, not buffered for later replay.
- [x] Preserve barge-in eligibility during normal speech. Exercise real provider
  paths so keeping the guard does not inadvertently disable interruption.
- [x] Use a10s bounded timeout to abort/recover, never to synthesize ack. Always
  clean waiters/tasks on disconnect and cancellation under Python3.10 semantics.
- [x] Run new tests, audio-bridge edge tests and existing lesson drain tests.
  Run the same suite in an isolated container using production dependencies;
  never modify the running container's Python environment to execute tests.

## Task 5: Repair measured PCM clipping and playback stalls

Files: S `core/voice/google_live/audio_bridge.py`,
S `tests/test_google_live_audio_bridge_edges.py`; F audio paths from Task1.

- [x] Add deterministic PCM tests for quiet, near-limit, full-scale positive and
  negative samples. Reproduce saturation using gain1.35 on25000 ->32767.
- [x] If effective gain/actual PCM confirms amplification clipping, remove unsafe
  added amplification or implement a reviewed continuous limiter. Prefer unity
  gain over a per-frame normalization algorithm that pumps loudness. Preserve
  configured attenuation and validate chunk-boundary continuity.
  Evidence obtained: isolated real Google Live response using fetched agent key
  and merged gain1.35 returned408962 samples/68 chunks, peak28454;469 samples
  exceed int16 range after multiplication. No robot output connected to probe.
  Select unity ceiling for added amplification, preserve attenuation; tests must
  prove chunk-invariant results and safe treatment of invalid/nonfinite gains.
  Implemented unity ceiling; independent spec/quality reviews passed. Baseline55,
  RED4failed/55passed, GREEN59passed on Python3.11. Exact production dependency
  image local/tbot-server:live-520c605c, isolated network-none/read-only source,
  Python3.10:59 assertions pass with pre-existing pending-task cleanup warning
  in invalid raw-generation test; PCM-only6 pass cleanly. No deployment.
  Test cleanup gap is unchanged in HEAD and bypasses the PCM gain path; repair
  test teardown before calling the complete integration suite clean.
- [ ] Identify whether slow output intervals arise inside write, around logs,
  decode scheduling or resource contention. Implement only the reproduced cause,
  with its own RED/GREEN regression. Existing750ms wall time alone does not
  authorize a speculative priority or driver change.
- [ ] Repeat identical audio before/after at applied65 and92. Report clipping
  counters and frame timing independently. If no cause is reproduced, keep this
  task incomplete and retain diagnostics; do not label instrumentation a fix.

## Task 6: Review, matched release and attended E2E

Evidence files: `/Users/manhhodinh/Documents/TBOT/robot/docs/stories/US-004-google-live-production-audio-gateway/validation.md`
and `/Users/manhhodinh/Documents/TBOT/robot/docs/TEST_MATRIX.md`.

- [ ] Review complete changes for stale callbacks, cancellation, DMA-tail proof,
  timeout recovery, full-duplex regression and secret leakage. Run native
  sanitizers, focused firmware/server suites and full firmware build freshly.
- [ ] Preserve rollback server image and app backup. Verify device MAC
 14:c1:9f:d1:ac:20 (current confirmed single robot), exact app hash, active
 partition and saved volume. Roll out
  matched capability support without making legacy clients wait for unknown acks.
- [ ] Execute ten30s replies +15s silence; require zero unsolicited self-replies,
  zero cut-off tails and no audible distortion/stutter confirmed by the user.
- [ ] Execute five real interruption/recovery turns and three comparable replies
  at each applied65/92. A requested100 still applies92; disclose that limitation.
- [ ] Exercise slow/lost ack and reconnect in isolated integration tests before
  attended device fault tests. Require bounded recovery, no false completion,
  no echo replay and no watchdog or stuck listening state.
- [ ] Report latency from user onset to output and final audible output to
  listening separately. Update evidence with exact versions, commands, counts
  and failures. Production-ready remains false while any acoustic gate fails.
- [ ] Commit only reviewed task files by explicit paths; leave unrelated
  display/GIF changes untouched. Do not merge or remove old worktrees implicitly.

## Self-review and execution choice

Spec requirements map to Tasks1-6. Handshake ownership, tail accounting,
compatibility, cancellation and recovery are explicit. Tasks1/5 intentionally
require measured evidence before selecting an audio repair; hardware root cause
is not invented in this plan. Firmware/server changes form one matched release.
User selected subagent-driven execution. Diagnostics, decode fence, pure drain
helper, codec DMA/error proof and PCM unity ceiling passed scoped reviews.
Server handshake implementation underway before firmware application integration;
this reorder leaves all new capability support inactive until a matched release.
Fresh merged robot config fetch reports output_gain1.35; this is not a snapshot
of running-session memory. Pre-integration firmware protocol/lifecycle baseline211
passed (lesson passive websocket/disconnect, internal RAM, drain, realtime state).
Production remains local/tbot-server:live-520c605c running; USB target node remains
present. Neither observation is an acoustic acceptance pass.
