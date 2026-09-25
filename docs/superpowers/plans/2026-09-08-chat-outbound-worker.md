# Chat Outbound Worker Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep Application responsive under blocked chat sends, with no stale uplink replay or false drain completion.

**Architecture:** One persistent chat outbound worker uses the existing packet queue and a bounded control mailbox, independently of Open/Heartbeat. Application owns intent and protocol lifetime; atomic invalidation precedes deferred cleanup. The existing drain controller supplies completion effects.

**Tech Stack:** ESP-IDF C++17, FreeRTOS, native clang sanitizers, pytest, Python server tests.

---

## Execution Constraints

Firmware root: `/Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware` (F).
Server root: `/Users/manhhodinh/Documents/TBOT/robot/esp32-server/.worktrees/agent-only-gemini-key/main/tbot-server` (S).
Paths below are relative to the indicated root. Continue directly in F as the
user requested; do not create another worktree or stage unrelated dirty changes.
Retain the previously selected sequential implementer/spec/quality review workflow.
No flash, capability enablement or production deployment before matched gates.

Written design: `docs/superpowers/specs/2026-09-08-chat-outbound-worker-design.md`.
Parent release plan: `docs/superpowers/plans/2026-09-08-google-live-playout-echo.md`.
This plan replaces the parent's ACK-only worker assumption, not its acoustic gates.

2026-09-08 baseline after Task0: full existing firmware suite excluding the new
in-progress mailbox file passes1637 tests in120.81s. Mailbox plus lifetime/reset/
controller focused run with CHAT_MAILBOX_TSAN=1 passes14 tests in13.47s. Counts
overlap and are not additive. No integration/flash inferred from these passes.

## Task 0: Close Reset Identity Review

Files F: `main/audio/audio_reset_epoch_publication.h`,
`tests/test_audio_reset_epoch_publication.py`.

- [x] Finish independent quality review after the completed functional spec review.
- [x] Clarify the existing comment: reset number 2^31 makes reads permanently Busy.
- [x] Run `python3 -m pytest tests/test_audio_reset_epoch_publication.py tests/test_audio_decode_fence.py tests/test_audio_playback_drain_snapshot.py -q`.
  Expected all pass, including configured Xtensa operation disassembly; record skips.

## Task 1: Bounded Outbound Mailbox And Cancellation Identity

Create F `main/chat_outbound_mailbox.h`, `tests/test_chat_outbound_mailbox.py`,
`tests/native/chat_outbound_mailbox_test.cc`. No Application wiring in this slice.

- [x] RED: compile and execute native assertions for two queued control slots,
  one active job, independent cancellation and a single retained completion.

```cpp
ChatOutboundMailbox mailbox;
auto generation = mailbox.AdvanceGeneration();
assert(generation != 0);
ChatOutboundMailbox::Job first{};
first.generation = generation;
first.request_id = 1;
first.protocol_generation = 1;
first.connection_epoch = 1;
first.kind = ChatOutboundMailbox::Kind::ListenStart;
assert(mailbox.TrySubmit(first));
auto second = first;
second.request_id = 2;
assert(mailbox.TrySubmit(second));
auto third = first;
third.request_id = 3;
assert(!mailbox.TrySubmit(third));
ChatOutboundMailbox::Job job{};
assert(mailbox.TryTake(job));
assert(job.request_id == 1);
auto replacement = mailbox.AdvanceGeneration();
assert(replacement != generation);
assert(!mailbox.IsCurrent(job.generation));
```

- [x] Run `python3 -m pytest tests/test_chat_outbound_mailbox.py -q`; verify the
  missing helper fails intentionally before implementing it.
- [x] Implement the fixed-capacity mailbox with `std::array`, bounded payload
  (max128-byte drain ID, no pointer/string ownership), and no platform dependency.
  Kind values: ListenStart, ListenStop, Abort, Wake, DrainAck. Job carries request,
  uint64 protocol generation, uint32 connection epoch/outbound generation and a numeric control
  argument. Payload rejects oversized and embedded-NUL data. Zero identities reject.
- [x] Application is the sole generation writer: `AdvanceGeneration()` uses
  atomic32 load/store only, returns nonzero current identity; UINT32_MAX is reserved
  as permanently revoked exhaustion. `IsCurrent()` rejects zero/exhaustion.
  Revoke never waits on the mailbox mutex or a worker. Do not represent it as a
  successful cancellation of already submitted wire bytes.
- [x] TrySubmit/TryTake/TryComplete/TryCollect use bounded try-lock operations.
  No mutex spans external work. TryTake skips at most the two stale queued jobs;
  one active job retains its identity until completion. One completion slot must
  be collected before another job starts. Failed attempts leave output untouched.
  Completion validates the active request and all ownership fields; duplicate or
  foreign completion rejects, and a stale result is marked stale on collection.
  Cancellation does not clear active ownership or a pending completion. Queue
  admission may compact obsolete queued jobs, never drop current ordered controls.
- [x] Add deterministic thread barriers: worker takes a job then blocks outside
  mailbox; Application advances generation and remains responsive; stale queued
  controls do not run; late completion collects as stale; new work then runs.
  Include full queue, duplicate completion, payload validation, identity exhaustion,
  try-lock contention and ordered listen/stop/abort tests with unchanged outputs.
- [x] GREEN run the pytest wrapper using actual header under ASan/UBSan; separate
  TSan invocation where supported. Spec review then quality review, no feature claim.

Task1 checkpoint: spec and quality reviews report no actionable findings. Root
combined14 tests pass with TSan enabled; independent reviewers each pass all5
mailbox tests. Initial missing-helper RED and unsupported Xtensa atomic trait RED
preceded corrections. Target probe qualifies actual generation load/store only.
This helper is not yet included by Application. Integration must use monotonic
request IDs (no reuse after collection) and recheck current intent before sends;
TryTake grants ownership, not irrevocable send authorization. Sanitizer barriers
cover handoff/contention, not exhaustive simultaneous generation stress.

## Task 2: Capture-To-Send Authorization

Read-only preparation: AfeAudioProcessor currently buffers input and output;
Stop clears input and calls SDK reset_buffer but does not clear output_buffer_.
Fetch waits indefinitely and returns no capture identity. A boolean stop/start
can miss an in-flight old fetch. The installed SDK header promises ring-buffer
reset only; configured Xtensa archive disassembly of afe_reset_buffer calls
sr_rb_reset, not a demonstrated full DSP/history reset. Do not infer that copying
the generation at OnOutput, or counting feed/fetch without overflow proof, solves
this boundary. Require serialized processor transition and explicit old-output
discard proof before opening a new generation. Legacy reference .ref source has
different APIs and is not evidence for the installed binary's full behavior.

Implementation boundary: retain AFE configuration and instance, serialize exit
of old feed/fetch plus clearing owned input/output/ring and resampler cached
frames before rearm. Tags prove no old captured frame is relabelled/replayed;
they do not prove zero internal DSP filter history in fresh processed samples.
Do not destroy/recreate AFE or label ring reset a full DSP reset. Residual acoustic
echo remains subject to parent hardware gates. Arm requires the current revoked
identity and matching worker clean-transition acknowledgement, never an old ack.

SDK-native concurrency refinement: keep feed and bounded-wait fetch concurrent;
serialize reset/prepare against both and against already-dispatched callbacks.
Do not hold the feed mutex during a blocking fetch. Do not poll fetch(0) every
millisecond: installed SDK logs on short/empty reads, creating a jitter risk.
Neither successful feed-call count nor get_fetch_channel_num proves raw ring-byte
availability for every channel layout. Retain fetch-entry tag and await old
fetch/callback exit before clearing buffers and publishing preparation ack.
App revocation remains lock-free; only the cleanup worker waits for transition.

Modify F `main/audio/audio_processor.h`, `main/audio/processors/afe_audio_processor.h`
and `.cc`, `main/audio/processors/no_audio_processor.h` and `.cc`,
`main/audio/audio_service.h` and `.cc`, `main/protocols/protocol.h`.
Create F `main/audio/chat_uplink_authorization.h`,
`tests/test_chat_uplink_authorization.py`, `tests/native/chat_uplink_authorization_test.cc`.

- [x] Inspect actual AFE feed/fetch/cache buffering before selecting metadata
  propagation; document how every output frame maps to capture authorization.
  A callback-time generation read is forbidden as proof of capture identity.
- [x] RED runtime barriers at capture, processor output, encode completion and
  send admission: capture A, invalidate, authorize B, release A; assert A is never
  submitted under B. Test mixed-generation frames are discarded, B survives, and
  test/lesson data never inherits chat authorization.
- [x] Add bounded single-writer atomic32 authorization, transport the capture tag
  through AudioTask and AudioStreamPacket and validate before encode enqueue,
  packet enqueue and wire submission. Revoke before any blocking cleanup.
  Preserve existing processor settings, capacities and qualified interruption.
- [x] GREEN native actual-method adapter plus processor/audio/realtime regressions;
  review metadata mapping and memory limits independently before continuing.

## Task 3: Persistent Worker And Lifetime Adapter

Task2 checkpoint: corrected production PrepareChatUplink makes acknowledged
token/scope immutable. Same-scope preparation is idempotent; changing scope
requires a new revocation. Actual-method tests now cover the full encode-queue
CV wait and interrupted preparation across revocation. Spec re-review passes;
quality review passes with no actionable findings. Root6 focused tests pass in4.55s and corrected full suite
passes1646 with2 default opt-in TSan skips in81.84s. Target build app0x38e770,
SHA256 b5cc1106ae90f6b059ec387e96194769dac606a59f7e0c8f21831eaf517b7288.
No wire-send integration, capability, flash or acoustic pass is inferred.
The Task2 send-admission helper is tested, but its actual wire-path use is deferred
to Task3; these checkboxes do not represent completed transport integration.
Root combined mailbox/capture/lifetime/controller run with CHAT_MAILBOX_TSAN=1
passes16 in11.06s (TSan coverage is mailbox; capture adapters use ASan/UBSan).

Task3 executes two sequential reviewed slices without changing its architecture:
3a adds explicit-result conditional transport methods and actual-method send
barriers; 3b connects the persistent worker and Application lifetime adapter.
3a has started. Existing legacy signatures/capability remain unchanged.

Task3a checkpoint: explicit-result conditional control/audio transport now passes
spec and quality reviews. Audio remains borrowed on Busy; matching socket lease
spans actual I/O and authorization is rechecked after formatting. C++ allocation
RED exposed escaping bad_alloc; correction returns Failed with RAII cleanup.
All five controls plus audio now exercise blocked replacement and late stale
completion. Root18 scoped tests pass in14.47s, full suite1647 passes/2 opt-in
skips in86.81s; subsequent test-only coverage correction passes root1 native
ASan/UBSan test in1.45s. Root target build passes app0x38eda0/10%free, SHA256
a7a9ce277b3acec6d8db460ba8f070e48bf667b9115e3cc6aef4fd81c91ba58d.
No worker activation or capability in this build. Task3b is next.

Task3b checkpoint: persistent worker and real Application admission/retirement
adapters implemented; normal chat activation remains dormant for Task4. Spec
review passes; quality review also passes without blocking findings. Root13 focused tests pass in9.97s with
CHAT_MAILBOX_TSAN=1 and CHAT_OUTBOUND_TSAN=1 (no skips). Fresh full suite1649
passes/2 default opt-in skips in88.03s. Root target build passes app0x391b80,
9% partition free, SHA256
12079461987538488473df42979661b7a25711b98a4fb18eed0a973ca29c8ad4.
ELF symbols/DWARF confirm stack8192 bytes, TCB352, worker object1016 (including
mailbox768); native host object1104 is a different ABI. No runtime high-water,
available-heap or latency proof without a matched flash. No flash/deployment.
Tests exercise actual app lifecycle methods queued/running across watchdog,
reset/reinit/reboot/close/open and deferred close. Deferred-close RED demonstrated
late retirement until poll; its callback now revokes before requesting close.
Existing ProtocolWorkLifetime reservation counter64 wrap remains a pre-existing
residual outside this slice; outbound generation32/request64 exhaust fail-closed.
Minor test follow-up complete: dedicated zero-deadline DrainAck and successful
same-ID/deadline admission Busy-retry assertions pass. Root final focused13 pass
in12.36s with both TSan flags enabled; production/hash unchanged. Native task wait shim proves
idle/10ms selection but not actual FreeRTOS notification coalescing/sleep-boundary
arrival. Retained-state ownership was reviewed; target scheduler behavior remains
a Task4/runtime gate. Reset/reinit publish Pending then synchronously Retire before
mutation; Retire is the cancellation boundary, not the preceding Pending store.

Read-only transport preparation: current listen/wake/abort Protocol methods return
void. Do not manufacture Sent merely because IsAudioChannelOpened remains true.
Add an explicit-result conditional chat sender on the worker path, preserving
legacy signatures, and retain the matching healthy/installed socket lease across
the actual send as with the reviewed ACK sender. Protocol lifetime alone does
not establish per-socket identity. Gate Busy retries must retain the original
request/deadline. Keep all failure callbacks out of blocking app cleanup.

Create F `main/chat_outbound_worker.h` and `.cc`,
`tests/test_chat_outbound_worker.py`, `tests/native/chat_outbound_worker_test.cc`.
Modify F `main/application.h`, `main/application.cc`, `main/CMakeLists.txt`.

- [x] RED hold fake transport SendAudio on a barrier while the Application
  adapter processes an independent clock event. Assert clock advances without
  releasing the send. Repeat for each supported control and heartbeat stall.
- [x] Allocate one persistent worker and coalesced wake notification; keep the
  existing audio queue as the sole packet backlog. Use mailbox control ordering
  before the next packet. No blocking queue pop or Send on the Application path.
- [x] Reserve an outbound activation through existing ProtocolWorkLifetime on
  Application before publishing its protocol pointer; return it only after the
  worker's final access and app completion. Count actual max reservations against
  the existing four slots; do not silently enlarge them. Pending teardown rejects
  admission, requests worker retirement, and never releases an in-use pointer.
- [ ] Handle task allocation/admission failure by visible recovery, retained
  cleanup and no capability. Keep Open/Heartbeat queue at two, independent of mic.
- [ ] GREEN execute actual adapter barriers for queued/running work, watchdog,
  reinit/reboot/close, late result and allocation failure; run lifetime regressions
  and full target build. Measure static stack allocation and available memory.

Task3b software foundation is reviewed. The two boxes above remain open only for
Task4 visible recovery/cleanup routing and target available-memory/latency gates;
native allocation/admission, lifecycle barriers and static layout already pass.
Clock evidence uses the actual inserted event adapter, not the entire MainLoop
with all legacy handlers. No full-Application nonblocking claim at this checkpoint.

Task3b preflight audit: StartOpenChannelWorker currently rejects any Busy lifetime;
retire an outbound activation before opening/replacing its connection, not just
on protocol destruction. Heartbeat uses the separate two-slot network queue but
does not dereference Protocol and does not itself reserve one of the four tokens.
Do not count its watchdog flag as protocol ownership. PopPacketFromSendQueue
takes the audio queue mutex; only the worker may call it on the new chat path.
ScheduleDeferredProtocolClose/CompletePendingProtocolWork still perform close on
Application once reservations retire; Task4 must defer that actual operation.
OnNetworkError currently invokes heartbeat/state logic directly on its callback
thread; marshal current-identity effects before enabling worker sends. Passive
liveness pings also share the socket; preserve the existing Busy exclusion or
move them off Application as part of the shared-socket blocking audit.
Application::Schedule currently allocates into an unbounded deque under its
mutex. Retained mailbox completion/activation retirement should use a coalesced
event and app collection, not one allocated Schedule callback per audio packet.
If delivery admission fails, ownership must remain retained for a later poll.

## Task 4: Chat Control And Deferred Cleanup Integration

2026-09-09 user approved4b continuation after4a reviews. Execute its prerequisites
sequentially: additive chat playback reset/admission refinement, callback source
identity, actual Application control/drain routing, then owned shared-socket
MCP/system frames and full integration audit. Keep the feature gate off throughout
these separate reviewed slices; activation belongs to matched Task5.

### Task4b First Slice: Preserve New Response Across Chat Reset

Actual-path evidence: legacy ResetDecoder holds audio_queue_mutex while waiting
for decoder_mutex. PushPacketToDecodeQueue takes that same queue mutex even with
wait=false. Deferring that reset alone can still stall receive before terminal
stop intake, and clearing all packets after the wait can lose a newly received
response prefix. Preserve legacy ResetDecoder; add a chat-only reset path.

Files: main/audio/audio_service.h/.cc, one focused chat reset fence header under
main/audio if needed, and tests/test_chat_playback_reset.py with an actual-method
native fixture under tests/native. Narrow Application cleanup hook may select
the additive method, but no normal-chat activation or callback routing in this
slice. Use the existing decode queue, capacities, codec settings and priorities.

Chosen additive boundary: AudioService::RequestChatPlaybackReset() publishes and
returns a bounded atomic32 request token before current-response admission;
ResetChatDecoder(uint32_t token) is cleanup-worker-only; IsChatPlaybackResetPending()
gates the codec predicate and decode branch. Requested identity and successful
completion are distinct; an old packet must carry the reset identity it claimed
before a decoder wait so a later completed reset cannot make it current again.
The existing reset serial must map explicitly to the request token rather than
silently replacing it with the latest request. Inspect SetDecodeSampleRate's
decoder close/open and mutable resampler state before choosing lock boundaries.
Admission must recheck nonzero chat tokens after acquiring the queue mutex and
after any capacity wait. Reclaim obsolete tagged packets before testing capacity:
a queue full of cancelled audio must not discard the new response prefix while
reset waits for the decoder. Preserve token-zero legacy entries and all current
entries; a queue full of current packets retains the existing bounded-full policy.
SetDecodeSampleRate closes under decoder_mutex but opens the decoder and updates
the resampler outside it. The codec transition lock must therefore serialize all
configuration/decode/resampler work, including legacy-token packets, against chat
reset. No queue mutex spans that lock wait. While chat reset is pending defer all
queued decode, not just tagged chat at the head; encoder readiness remains
independent. A dequeue-time requested-reset watermark also fences legacy packets
already claimed before a chat reset, so they cannot mutate a fresh decoder after
the wait. Preserve queued legacy data and normal no-chat-reset behavior; neither
the transition nor packet metadata proves recall of an in-flight hardware write.
Capture that watermark before the pending guard and dequeue, not afterward:
a request between dequeue and a later load must not relabel an old legacy claim.
The service-issued token is also the Application cleanup serial: one request per
reset, unchanged for nonreset intents, copied immutably into worker obligations.
Reset cached output-resampler samples under transition exclusion; failure leaves
the request pending and preparation revoked even if Opus reset already succeeded.

2026-09-09 first reset-slice candidate: root full1660 passed/2 opt-in skips in
98.17s; focused38 passed in32.74s with all three TSan flags, including configured
Xtensa request-operation proof. Target app0x393760/9%free builds. Spec review is
NOT PASS: a request between dequeue and watermark capture can let old token-zero
work mutate a freshly reset decoder. Correct with a deterministic regression
before re-review; these passing tests do not close the finding or release gates.

Watermark correction now passes scoped spec re-review, with a post-dequeue
request/reset/late-claim deterministic regression. Root corrected full1660 passes
with2 opt-in skips in91.72s; focused38 pass in28.74s with all three TSan flags.
Corrected build remains0x393760/9%free, SHA256
5d7c4deae961bbf7f4a172bfd4de9f586fa86726aae02c33d80817e6ccbf4ec1.
Quality review remains open, including server-AEC timestamp handling for skipped
output. Target DWARF: AudioService648, Application2600, reset fence8, packet44,
PCM task40 bytes. These sizes are not runtime heap/stack-headroom evidence.

Quality review confirmed the server-AEC metadata defect: a skipped tagged write
still queued its timestamp. A macro0/1 actual-output regression reproduced RED
with AEC enabled. Timestamp publication now requires current_output; current
tagged and legacy writes retain their timestamps. Spec recheck PASS; final
quality re-review and root regression are pending. Diagnostic playback counts,
timing and last-output time still include skipped tasks; do not treat them as
actual write/acoustic measurements. This candidate does not change AEC config.

Final2026-09-09 corrected prerequisite: spec and quality re-review PASS. Root
full1661 passed/2 opt-in skips in99.84s; focused40 passed in35.19s with all three
TSan switches. Target build0x393760/9%free and SHA256
5d7c4deae961bbf7f4a172bfd4de9f586fa86726aae02c33d80817e6ccbf4ec1.
This supersedes earlier candidate evidence, not the remaining runtime/release
gates. No source identity/routing/MCP activation, serial, flash or deployment.

- [x] RED hold the actual decoder mutex and enter chat reset, then enqueue
  current-response packets through the actual admission method without releasing
  the decoder. Verify app atomic invalidation can progress;
  do not mistake a scheduler timeout for proof reset entered its blocking point.
- [x] Publish pending reset before allowing the new response. Prevent dequeue
  and decode of current packets while reset is owed; include the pending check
  in both wait predicate and decode branch so microphone encoding is neither
  starved nor replaced by an idle busy loop.
- [x] Release queue ownership before decoder wait. On completion discard only
  obsolete response data, preserve current prefix order, and reject old in-flight
  decode completion. A delayed older reset cannot clear a newer pending reset.
  Failed/saturated resets remain fail-closed; existing bounded capacity/full
  admission behavior is unchanged. No extra PCM/Opus backlog or prefix dropping.
- [x] GREEN actual-method barriers cover reset failure/retry, multiple reset
  requests, current prefix decoded exactly once after successful reset and legacy
  reset behavior. Audit queued/dequeued
  old PCM before output without claiming recall of already-submitted DMA audio.
  Run python3 -m pytest tests/test_chat_playback_reset.py
  tests/test_audio_decode_fence.py tests/test_audio_reset_epoch_publication.py -q,
  sanitizers and target build, then independent spec and quality reviews.

The scoped reset prerequisite is reviewed; original-stop clock progress through
real Application intake and no-reset-after-proven-drain remain required in the
following runtime routing slice. Neither is claimed by these reset adapters.

### Task4b Second Slice: Source-Bound Callback Foundation

Continue directly in F, one implementer then spec and quality reviews. Files:
main/protocols/protocol.h/.cc, websocket_protocol.h/.cc, a focused connection-source
value header if needed, main/chat_protocol_signals.h, narrow dormant Application
source-selection/error/close adapters, and native callback/transport tests.
Keep legacy callback signatures and MQTT behavior; do not invent a WebSocket
Connected event. Current Connected is MQTT-only. No capability or runtime routing
activation, serial, flash, deployment, or controller/MCP rewrite in this slice.

Use an immutable value ConnectionSource{source_id, connection_epoch}. Allocate
source_id monotonically under the existing connection-mutation gate, saturating
fail-closed; separate per-protocol signal objects fence cross-protocol collisions.
The socket source is distinct from unchanged uint64 lesson transport epoch.
OnData/OnDisconnected capture this value at closure creation. Opened/handshake
errors use the same source. Explicit close retains its socket source before
failure mutation/destruction, rather than reporting the new failure epoch.
If an installed socket A remains while candidate B is handshaking, explicit
physical-close notification names A; with no installed socket it names B.
Carry the chosen source through teardown while the failure epoch separately
guards notification ordering. Exhausted source allocation rejects Open before
connection/callback publication and leaves the gate unhealthy.
Conditional send errors use the leased source; no later CurrentConnectionEpoch
lookup may relabel an old event. Legacy-only error entry points cannot silently
swallow a selected transport fault: audit all selected paths before activation.

Additive source callbacks select source-aware delivery when registered, legacy
delivery otherwise. Audio/JSON transport delivery carries source, but actual
Application response/controller intake remains the following slice. Dormant
source Error/Closed adapters publish only to the captured per-protocol signal
object, never dereferencing Application's movable protocol pointer. Source
selection is explicit, app-owned, and disabled by cleanup; callbacks cannot write
eras or auto-enable. A source selection API is not proof its caller has a fresh
connection: runtime activation must validate successful current Open/lifetime.
Selection rejects previously selected source IDs; once source-aware mode is
selected, source-less Publish cannot bypass it, including after disable/enable.
Install source callback functions only before transport Start/Open, never race
std::function mutation with invocation. No per-attempt shared heap object is
needed for the value source; fixed callback closures still have measured cost.

- [x] RED actual transport callbacks delayed across replacement cannot be
  relabelled. Test audio/JSON/Opened/Closed/error source delivery, separate lesson
  epochs, explicit-close source retention and legacy/MQTT compatibility.
- [x] GREEN value source allocation/capture/dispatch and dormant app signal
  adapters; source exhaustion stays disabled. No per-packet allocation/queue.
- [x] RED/GREEN real Connected/Opened Application closure adapters, current and
  stale queued effects. Force Publish/Collect mutex contention with deterministic
  barriers: Busy retains flags; disable/source switch rejects old publication.
- [x] Focused sanitizer/target probes, full regression, ESP32 build and independent
  spec/quality review; update US-004 evidence. No runtime/acoustic PASS inferred.

2026-09-09 frozen source foundation: spec and quality re-review PASS after
test-only adapter corrections; production unchanged. Root full regression1667
passed/2 opt-in skips in105.99s; combined source/app/conditional transport/gate/
reset/worker/mailbox43 passed in38.41s with all three TSan switches. Fresh target
build passes app0x393f30/9%free, SHA256
c575a985c4630092b6cb6d050abae6ca6902320e09668ad9ca117e90d86a4e30.
Source transport Opened fixture calls delivery manually, not the complete Open
sequence; actual Application success closures are extracted separately. No full
source Open runtime, callback heap/headroom, or target scheduler proof. Conditional
audio failure remains an explicit worker result; legacy drain ACK generic send
must not be selected for the new path. Application audio/JSON/controller routing
and bounded owned MCP messages remain pending. No activation, serial, flash,
deployment or acoustic PASS.

### Task4b Third Slice: Original Terminal Stop Handoff

2026-09-09 continuation approved. Implement directly in F with one implementer,
then spec review and quality review. Root maintains evidence docs. Start with a
response already established by Application; keep construction/registration
dormant. Source-aware tts:start, replacement/prefix ownership, full state/listen
routing and MCP remain subsequent integration work, not silently activated here.

Preflight rejected a one-ticket rule that rejects every additional tts:start:
the current server audio bridge cancels/replaces pending normal stop on a new
audio_start. The wire start has no response ID to distinguish duplicate from
replacement. Do not impose that behavior restriction or introduce callback-side
reset requesters. Application remains sole reset/cleanup/controller owner.

Files: main/application.h/.cc, main/chat_protocol_signals.h and the focused
main/chat_playout_intake.h; tests/test_chat_playout_intake.py with
native/chat_playout_intake_test.cc and native/chat_terminal_application_test.cc.
Existing callback/worker/cleanup fixtures adapt the additive dormant methods;
tests/test_chat_uplink_authorization.py adds a configured Xtensa intake probe.
Retain immutable app-established source, protocol generation, connect intent,
response generation and reset token through callback handoff. Terminal stop owns
up to128 drain ID bytes, the original callback receive timestamp and the result
of exactly one TryGetPlaybackResetEpoch capture. A Busy original capture fails
closed; a later successful read must not replace it. Preserve duplicate stop's
original clock. Malformed current stop fails closed; stale stop cannot fault a
new source/response. Keep JSON memory ownership within callback parsing.
Retain continue_listening/listen_mode intent in the owned record. A qualified
interrupt without drainId is cancellation, not a malformed normal ACK. Interrupt
after normal stop must override ready/ACK eligibility; only an identical normal
duplicate may preserve first-record semantics. Conflicting terminal data cannot
silently become a duplicate or authorize relistening.

Use bounded retained control state, not per-packet Schedule allocations or a
second audio backlog. App polling never waits on callback mutex, audio snapshot,
network send or decoder reset. An uncollected stop behind mailbox contention must
not hide its deadline: publish a bounded independently readable pending/deadline
or fail closed conservatively on an unreadable current terminal handoff. Never
report timeout before callback receipt based on bytes queued at server/network.

Embed the intake in the existing per-protocol ChatProtocolSignals allocation;
callbacks retain that object and app polls only the current one. WebSocket OnData
holds ConnectionInboundGate's recursive mutex across dispatch, providing one
non-reentrant terminal-publication writer per object. Do not publish terminal
records/fault stamps from Error/Closed or Application. Application alone publishes
response identity; it never clears callback-owned stamps. Monotonic response
stamps and separate per-protocol objects prevent delayed old stores from hiding
new faults. Retain bounded atomic32 load/store publication rather than adding
unqualified Xtensa read-modify-write operations. Verify serialization and stale
cross-protocol cases, measure increased per-protocol allocation layout.
Contention must be attributed to the captured response: a current initial writer
paused before publication fails closed, but an exclusively old writer holding
the mutex across atomic Establish(new) must not fault/reset the successor.
Current source/connect loss revokes the same response; obsolete response/reset
loss cannot reset its successor. Current Error/Closed must invalidate pending ACK
and retained ready; source-scoped fault identity must not erase or relabel sticky
infrastructure faults during a later successful source selection.

Controller SubmitAck maps to an owned ChatOutboundMailbox job with the original
deadline. Admission Sent is not transport delivery. Correlate actual worker
completion to both controller request and full current identity. Fresh drained
snapshot is required on completion. Complete produces only a retained app-ready
effect in this slice, not a direct listen send, mic arm or decoder reset. Cancel,
failure and deadline retain recovery requirements and revoke old input/playback
without invoking legacy blocking state handlers; defer actual cleanup.
Retain original deadline and complete identity with the ready effect for the
subsequent listen-delivery consumer. Ten seconds through blocked listen delivery
is still a full-routing gate, not proved by this terminal-only slice.

- [x] RED actual stop parser/handoff:128-byte ownership,129-byte rejection,
  stale source/intent/response, duplicate deadline, original reset capture Busy.
- [x] GREEN bounded retained intake and Application controller/ACK correlation;
  no source start registration, per-packet allocation or normal-drain reset.
- [x] RED/GREEN hold mic/ACK send and terminal mailbox; exercise Busy audio
  observations in the terminal adapter with real snapshot locks tested separately;
  advance original clock to10000000us and prove fail-closed app progress without
  releasing the barrier. Late Sent never produces ready-to-listen.
- [x] Run focused actual-method ASan/UBSan/TSan, existing callback/controller/
  reset/lifetime/lesson regressions, full pytest and target build; sequential
  independent spec/quality reviews, record exact coverage and remaining gates.

2026-09-09 final terminal candidate: spec and quality re-review PASS after
initial-publication visibility, stale-writer contention, same-response ownership
loss and pre-stop/current-ready fault corrections. Root full1671 passed/2 opt-in
skips in135.90s; focused54 passed in55.36s with all three TSan switches. Fresh
target app0x3950a0/9%free builds; SHA256
5ad7c1b0877c8011a0a9cc167a2745066e4a217ca0d5dd3fec7d9572931f4cf2.
Target Application3512, Signals288, Intake248, Stop200, Controller256 bytes;
shared-allocation overhead/runtime headroom remain unmeasured. Xtensa
Establish/Current/TryCapture probe checks scalar acyclic control flow and absence
of atomic runtime/RMW, allowing compiler-generated fixed36-byte memcpy/memset;
it does not establish all-call-free execution or target scheduler timing.
Terminal fixture uses actual Application/controller/worker and cJSON, but stubs
cleanup and audio snapshot internals; separate actual suites support these
boundaries, not one combined end-to-end callgraph. No new start/audio registration,
listen/rearm/visible recovery, MCP integration, capability, flash/deployment or
acoustic PASS. Full Task4b remains open.

### Task4b Fourth Slice Preflight: Start Admission Decision

2026-09-09 preflight completed; user approved bounded 250 ms admission. Implement
the dormant start/prefix slice directly in F, then sequential spec/quality review.
The server audio_start cancels a pending normal stop and emits tts:start without
a wire response ID. Every valid replacement must remain admissible; do not revive
the rejected single-ticket restriction.

Approved option: one fixed start request
embedded in the existing per-protocol signals. The serialized receiver publishes
immutable source/protocol/connect/start-serial identity and waits only for bounded
Application admission, never decoder reset or worker readiness. Application alone
allocates the response/reset identity and publishes pending reset before releasing
admission. Subsequent binary packets retain that admitted identity in the existing
decode queue. Proposed admission budget is 250 ms, not a target-measured latency
claim. Expiry must revoke late admission and retain source-owned recovery.

The Application path must never wait on the receiver gate held by this callback.
Playback-only deferred cleanup must preserve qualified realtime interruption;
the existing revoke-and-prepare cleanup cannot be reused indiscriminately here.
Replacement invalidates old outbound work but retains its ACK correlation and
reservation until real retirement. No second PCM/Opus backlog, priority/buffer
changes, callback-side reset writer, serial, flash, deployment or motor actions.

Alternative: receipt-tag existing queued packets before Application admission.
This avoids a new receiver wait but adds codec-admission identity and unresolved
pre-reset claim/config fencing. It is a larger design, not selected implicitly.

After the start decision and reviewed implementation, continue state-owned
ListenStart: exactly one admission, current real delivery and audio preparation
before input arm, all within the original stop's ten-second deadline. A proven
normal drain uses preparation without decoder reset. Integrate completion routing
with the existing bounded collector rather than an independent consumer.

Execution checklist (this slice does not implement listen/rearm or registration):
- [x] Add native RED tests for fixed start admission, 250 ms expiry/late admit,
  replacement serials, stale source/intent and exhaustion. Use deterministic
  barriers and fake clock; no scheduler sleep as the correctness oracle.
- [x] Implement main/chat_start_handoff.h embedded in chat_protocol_signals.h.
  Application try-collect never waits on receiver-owned locks. Receiver waits
  only for matching admission; current expiry retains recovery, stale expiry
  cannot recover a successor. Keep atomic32 target constraints.
- [x] Add actual Application RED tests for start -> pending reset -> admission
  -> initial audio and replacement while prior ACK remains in flight. Extend
  real playback queue/reset coverage so prefix preservation is not a stub claim.
- [x] Implement source-bound start/audio adapters in application.h/.cc and
  playback-only cleanup through the existing audio worker. No Revoke/Prepare/
  Stop on ordinary realtime replacement; reset obligations survive contention.
  Retain old ACK ownership until real worker retirement, with bounded storage.
- [x] Run python3 -m pytest tests/test_chat_start_handoff.py -q, then affected
  playout/outbound/source/playback/cleanup suites with sanitizer opt-ins. Expected
  PASS; record initial RED assertions separately from fixture compile repairs.
- [x] Independent spec review, fixes/re-review, then quality review/fixes.
- [x] Root fresh full pytest and ESP32-S3 build, diff check and layout measurement;
  update validation/execplan. No commit, flash, deployment or acoustic PASS.

Fourth-slice final evidence: spec re-review and quality review PASS. Two review
findings received behavioral RED/GREEN: successful uncollected preparation A
must retain readiness obligation when reset B supersedes it; accepted STOP must
seal receiver audio while preserving duplicate/conflict timestamp identity.
Tests retain new START reopening and stale source isolation. Additional RED/GREEN
covers initial admission, full-cleanup incorrectly revoking realtime input,
and exhausted start serial with no previously established playout response.

Root full1673 passed/2 opt-in skips in120.90s; focused57 passed in56.42s with all
three TSan switches, no skips (counts overlap). ESP32 build0x395b00/9%free,
SHA256 f32a06554bd57fdda43e2b66f1dfdcf91dd420c93742b1caaa4a36fa45810a40.
DWARF Application3736, Signals376, Handoff56, Request32, Admission8 bytes.
Actual app fixture stubs audio internals; separate real reset/queue/helper tests
prove prefix retention with decoder held, and actual cleanup methods preserve
capture preparation. One receiver/app handoff uses separate host threads; most
deadline tests use deterministic clock hooks. These are not a combined full-loop
or target scheduling/headroom/acoustic proof. Source adapters stay unregistered;
listen/rearm, visible recovery and full JSON/MCP routing remain next work.

### Task4b Fifth Slice: State-Owned Listen/Rearm And Visible Recovery

Subsequent Sixth control entry integration is locally complete and reviewed:
`2026-09-09-chat-control-integration.md` Task1. Root1679/2 opt-in skips and
focused65 all-three-TSan pass; target/hash in US-004 validation. The old selected
synchronous Stop/mic/wake/abort/watchdog entry gaps are now covered; full source
JSON/MCP/shared-socket/lifecycle activation remains pending. No physical PASS.

2026-09-09 continuation approved. Direct F implementation, one code/test writer,
then spec and quality review. No callback registration/capability, serial, flash,
deployment or motor actions. Root owns docs; no commits or worktree operations.

Modify main/application.h/.cc and focused native/pytest fixtures. Retain one
app-owned rearm phase (None, Pending, Armed, IdleComplete, Recovery), one owned
ListenStart job and preparation token; reuse the current response/stop identity.
No independent completion collector or new queue/task. Actual state handler owns
bounded submit/retry and arm progression before rendering. Isolate selected chat
state rendering from legacy Listening/Idle audio/network side effects.

Continuation eligibility is sampled before post-drain revocation: current
source/protocol/connect/response/reset plus online, non-passive, non-lesson voice
turn in Speaking/Listening. continue=true requires existing voice ownership;
choose realtime only when explicitly requested, otherwise existing default.
Without continue, Speaking+Realtime resumes, Speaking+Manual/Auto ends Idle;
explicit manual stop ends Idle. Never turn passive/lesson into chat authorization.
Pending stays Speaking (Listening->Speaking is a valid transition) and displays
existing PLEASE_WAIT, not LISTENING. Do not begin gestures for this transition.

Normal rearm requests preparation once with reset=false, processing=true. Add
owned cleanup wake policy Explicit/Listening; the worker alone resolves the
configured listening AFE wake behavior, since IsAfeWakeWord takes a mutex.
Actual delivered Sent plus current prepared token and completed current playback
reset are all required before ArmChatUplink. Admission alone never means delivery.
Retain original stop+10s through mailbox contention, send and preparation; no
retry extends it. Consume ready/deadline only after successful arm. Intentional
IdleComplete cancels rearm deliberately with no listen/no decoder reset; it must
not spuriously time out ten seconds later. Preserve stop for duplicate/conflict.

Current faults/interrupt/conflict still revoke after Armed/IdleComplete. Fresh
START cancels old rearm ownership without resetting successor or muting ordinary
realtime playout; old listen completion/reservation uses the existing bounded
obsolete-generation retirement path. Foreign lesson/state/source ownership cannot
be repainted/reset by old events. Recovery revokes immediately, requests deferred
cleanup and transitions to isolated Idle rendering, never legacy blocking Idle.
Display/state-listener lock latency remains outside full-runtime hard-bound proof.

- [x] RED actual state-handler path: duplicate events/Busy reuse one ListenStart
  identity; no synchronous send/reset/wait fallback. Add native rearm scenarios
  to terminal/application fixture or focused tests/test_chat_rearm.py as needed.
- [x] GREEN bounded phase progression and sole existing completion routing.
- [x] RED/GREEN actual worker blocked ListenStart, failure/stale/late Sent,
  preparation-first and delivery-first, exact original10s deadline, successful
  arm followed by late clock/duplicate STOP, current conflict/error after arm.
- [x] RED/GREEN real cleanup preparation and Arm preserve normal decoder drain;
  compile wake policy enabled/disabled, execute policy resolution on worker.
- [x] RED/GREEN mode/intent matrix, visible pending/recovery, new START/source
  during blocked listen or preparation cannot authorize/recover a successor.
- [x] Spec review and corrections, then quality review and corrections.
- [x] Root fresh full/focused sanitizer pytest, ESP32 build and layout/hash;
  update harness evidence. Keep full JSON/MCP/wake/watchdog activation audit and
  matched release/physical acoustic acceptance explicitly open.

Fifth-slice final: spec re-review and quality PASS. Behavioral RED/GREEN fixes
include falling into legacy state effects, interrupt arriving between delivery
poll and Arm, foreign Idle cancelling Pending/Armed, and successor START retaining
the same conversation's pre-revoke intent. Negative transfer tests cover new
source, offline, passive, lesson and Idle; no stale eligibility propagation.
Actual HandleStopListeningEvent is exercised before the rearm state handler, but
its legacy synchronous SendStopListening remains unchanged. Owning ListenStop's
control lifecycle is a subsequent activation gate, not silently claimed done.

Root full1674 passed/2 opt-in skips in141.76s; focused59 passed in73.50s with all
three TSan switches (no skips, overlapping counts). ESP32 build0x396610/9%free,
SHA256 40ec337873da7086d7ced1222cba3b45365d970f7e5e47a2bc046db2c11dd26e.
DWARF Application4192, Signals376, ChatAudioCleanup20 bytes. Diff check clean.
Actual selected handler prefix, Advance/Render/Poll and outbound worker execute
in the native fixture; legacy remainder is trapped. Combined app audio readiness,
Arm and display/mapper are stubs, supported by separate actual cleanup/Prepare/
authorization tests with wake policy0/1. Not combined full-loop, target rendering,
headroom, scheduler or acoustic proof. Feature remains dormant/unregistered.

### Task4a Checkpoint And Remaining Task4b Contract

2026-09-08 continuation approved. Execute two sequential reviewed checkpoints:
4a retains app-owned audio/transport cleanup obligations. A dedicated serialized
audio cleanup task is independent of blocked outbound and Open/Heartbeat I/O.
Transport teardown uses an additional work kind on the existing two-slot network
worker; queue-full admission remains owed, and pointer ownership transfers only
after all real protocol reservations retire. A stalled heartbeat may delay
physical teardown, never app recovery. Completion returns publication/reinit/reboot
effects to Application. No new normal-chat activation in this checkpoint.
4b wires actual chat control/drain/state routing, input readiness, callback fencing
and separately bounded owned MCP messages (not the128-byte control field).
No capability, flash or deployment until matched release gates.

4a readiness audit adds narrow AudioProcessor::IsCaptureReady qualification for
the new cleanup transition, preserving legacy Initialize signatures. AFE missing
event/config/interface/data/task fails closed with guarded teardown; chat does not
retry a partially initialized processor. Requested wake state is checked after
the existing wake transition instead of inferring success from its void return.
New audio request/reset serial exhaustion and worker exceptions must retain a
failed cleanup result without authorizing mic or claiming decoder reset success.

4a continuation audit: failed decoder resets remain owed and may retry once per
existing clock tick, never on an immediate completion hot loop. A deterministic
test must establish entry into the actual reset boundary before proving app
progress while decoder/queue locks remain held. Existing ResetDecoder holds the
queue mutex while awaiting the decoder; do not claim that mutex is released.
Protocol callbacks may capture one bounded shared signal object allocated during
protocol initialization, not during cleanup admission or callback delivery.
Application polls only its current object; callbacks must not read the movable
Application protocol pointer. Allocation failure rejects the new path, and stale
or cleanup-generated notifications must not resurrect a restored connection.
Measure this per-protocol heap cost separately from static worker storage.

2026-09-09 Task4a software checkpoint: spec re-review PASS after correcting
Close-to-destructive escalation bookkeeping once per context, including queued,
completed and failed Close. Native counters cover all three destructive actions
and admission retries. The old callback cancellation source contract is updated
to require current era and lifetime checks before application-side cancellation;
actual current/stale close adapters preserve that behavior. Root full regression
passes1654 with2 default opt-in skips in89.79s; focused application/mailbox/worker/
capture/lifetime tests pass26 in21.46s with all three TSan flags enabled. Target
build passes app0x392e80/9%free, SHA256
2756c46617ee3c5fea0c9d2366e777069fd7d923962179a0dc163028188459db.
Quality review PASS with no blocking findings. No capability, cleanup task initialization or normal
chat activation yet. Current ELF Application2592, Signals20 and both cleanup
contexts16 bytes each; shared allocation overhead is additional. Audio cleanup
stack8192 is declared but linker-elided while initialization is unused. No target
heap/headroom or scheduler proof. Reboot retains lifetime admission through audio
Stop completion and its one-second countdown; Stop completion does not prove all
audio service workers have exited.

Review residuals for4b: actual Error/Closed callbacks are extracted in native tests;
Connected/Opened era guards are code-reviewed without equivalent closure adapters.
Signal stale-era tests do not force Publish/Collect mutex contention. Add these
cases when qualifying actual callback activation; existing sanitizer passes are
not evidence for unexercised interleavings or target notification scheduling.

Task4b must also finish the callback ownership audit: JSON/unpair directly reads
the movable app protocol pointer and shares socket sends, while other scheduled
JSON effects need current intent fencing. Error/closed/opened/connected era tests
are not proof for those paths. Signals stay disabled after cleanup. Re-enabling
before arbitrary Open admission is insufficient when an old socket still exists;
require source connection identity or proven prior physical teardown. Never enable
the new path merely because the Task4a foundation passes.

Preflight actual-path details: incoming terminal stop currently accepts drainId
only up to64 bytes, waits/sends ACK in a scheduled lambda, then may send a second
listen-start directly before the state handler sends its own. New chat drain
intake must use the agreed128-byte owned bound and original callback receive time;
keep legacy lesson parsing behavior separate. The interrupt callback currently
calls ResetDecoder synchronously. HandleListeningWatchdogTick also obtains queue
depths then sends/stops/pops on Application; it needs the same chat fail-closed
branch, not only the main SEND_AUDIO event. OnAudioChannelClosed has queued
blocking backlog drops and Idle handlers call processor Stop/wake controls.
EnableVoiceProcessing(true) resets the decoder unconditionally; do not use it
after a proven normal drain without auditing its effects. Preserve processor
initialization/start readiness and defer all blocking preparation before arm.

Read-only preparation: Application::SendMcpMessage schedules synchronous protocol
send on Application. It is reachable from tool responses and shares the socket;
it must be covered by the indirect blocking audit, not silently left behind.
The small control mailbox's128-byte text payload is not an MCP payload limit.
Preserve existing MCP response sizes through a separately bounded ownership path
when integrating; do not truncate tool messages into that control field.
Current tools/list pagination is2500 bytes before JSON-RPC/session wrapping;
ImageContent and other tool results are not bounded by that pagination constant.
Do not infer a universal MCP payload ceiling from tools/list. Bound queued message
ownership/count and handle allocation/admission failure explicitly without silent
truncation or an inline socket fallback.

Modify F `main/application.cc`, `main/application.h`,
`main/audio/audio_service.cc`, `main/audio/audio_service.h`.
Create F `tests/test_chat_outbound_application.py`,
`tests/native/chat_outbound_application_test.cc`.

- [ ] RED exercise actual MAIN_EVENT_SEND_AUDIO, Listening entry, wake/abort,
  stop controller and error handling. Hold Send or decoder mutex; advance the
  original stop clock to10000000us; require non-listening recovery without ACK.
- [ ] Route every normal-chat send through worker admission. Audit other shared
  socket sends reachable on Application so an unrelated control cannot block
  the same recovery event. Keep lesson/legacy ordering explicitly isolated.
- [ ] Wire original-stop identity and ConversationPlayoutController effects.
  Ack Sent is not enough: require fresh ownership and drain snapshot. Only the
  state handler admits one listen-start; authorize input after current delivery
  and audio readiness, never through the old force-listening timeout fallback.
- [ ] Publish input/playback invalidation immediately on timeout/cancel. Maintain
  a persistent cleanup obligation serviced independently of blocked outbound I/O.
  Actual decoder reset/close/destruction is deferred and lifetime-safe. Full
  admission queues retry without inline fallback or clearing the obligation.
- [ ] GREEN prove cleanup exactly once, late ACK/listen cannot rearm, real
  interruption survives, no tail truncation on normal completion, and all legacy,
  lesson, wake, activation and gesture regressions pass. Separate spec/quality review.

## Task 5: Matched Release And Physical Proof

Read-only release inventory check: current S runtime closure identifies Python3.14
and contains zero conversation_playout resource paths; deterministic node list
also lacks that suite. The recovered3.11 test runtime is useful local evidence
only. Regenerate and review candidate-bound inventories through the established
release tooling with the intended runtime, not manual edits that bypass gates.

Modify F `main/protocols/websocket_protocol.cc` only after worker gates pass.
Verify S `core/voice/google_live/conversation_playout.py`, provider/audio bridge,
matching tests and release closure inventory using the parent release procedure.

- [ ] RED/GREEN server tests: late wire ACK after expiry/cancel cannot release a
  newer response; actual matched firmware effect traces preserve echo suppression.
- [ ] Enable capability only for supported codec and initialized worker path;
  check legacy no-capability peers remain functional without acoustic pass claims.
- [ ] Fresh firmware regression/sanitizers/build, exact-production server tests
  and Git-bound release inventory. Record hashes and rollback artifacts.
- [ ] Flash only the verified current target14:c1:9f:d1:ac:20 after matched deploy;
  preserve NVS/assets/volume/model/key and verify image/boot readiness.
- [ ] Execute parent attended tests (10 replies/silence,5 interruptions,65/92
  comparisons), timing and wake checks. No production-ready claim on any failure.
- [ ] Update US-004 validation, TEST_MATRIX and architecture decision with actual
  counts, remaining gaps, allocation measurements and deployment state. Commit
  only explicit reviewed task files when the integration set is ready.

## Coverage Review

Ownership/mailbox maps to Tasks1/3; capture identity to2; control/deadline/cleanup
to4; compatibility/memory/release/acoustic gates to3/5. Task0 closes a prior
prerequisite and never represents the outbound feature. Detailed adapter code
is derived from actual methods in each RED test, not a second invented app model.
No feature or release task is complete merely because its standalone helper passes.
