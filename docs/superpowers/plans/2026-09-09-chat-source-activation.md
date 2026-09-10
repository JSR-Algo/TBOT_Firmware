# Complete Chat Source Activation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Complete source-aware message/control/lifecycle routing before enabling the approved Google Live drain capability on a matched release candidate.

**Architecture:** Keep the single outbound worker and existing response mailbox. Add two bounded logical connection-message records with full owned payloads and exact physical-job retirement tracking, plus bounded owned inbound JSON. Construction-time source callbacks preserve all legacy message routes and lifecycle effects; only complete initialization permits capability advertisement.

**Tech Stack:** ESP-IDF 5.5, C++17, cJSON, existing WebSocket, native barrier tests and pytest.

---

Approved continuation after reviewed Sixth control integration. Direct firmware
checkout, root owns docs, sole implementer owns code/tests. Preserve dirty work;
no commits, worktrees, server deployment, serial, flash or motor commands during
implementation/review. This is one coherent activation task, not permission to
install the current START/STOP-only callbacks before the remaining routes work.

User approval2026-09-09: after completion and independent review, root may create
scoped local commits to bind release evidence to Git. No push or merge authorized
by this approval. Incomplete firmware activation must not be committed as a
release-ready candidate. Server and firmware review/verification remain separate.

## Fixed Ownership And Delivery Contract

Two connection-owned logical records include active records. Each owns immutable
full encoded text, logical ID, ConnectionSource, protocol/connect identity and
original receipt+10s deadline; physical request ID/generation/reservation are
separate. Outbound full envelope limit is the existing65535-byte WebSocket limit;
reject oversize explicitly, never truncate. Do not change128-byte response-control
storage, existing mailbox capacity, audio queue capacity or task priorities.

Replacement START supersedes response controls/audio but preserves accepted
same-source connection records. Keep single sender, shared completion dispatcher
and existing lifetime reservation. Full-text transport authorizes immutable
source/protocol/connect ownership instead of response generation. Final exact
source gate remains held through send. Existing response authorization unchanged.

Physical outcomes:

```text
Sent -> terminal success once, even after response generation changed
Failed/possibly-submitted exception -> terminal failure, never replay
Busy/Stale -> guaranteed no send started, retry only with same source/deadline
no completion + exact recorded reservation retired -> queued-unsent; may rebind
source/connect/protocol replacement -> cancel logical record, no successor replay
```

The queued-unsent proof relies on CompactQueue dropping only untaken work;
active work always completes before TakeRetired/TryIdle. The sole dispatcher must
collect/route full-text completion before releasing reservation or passing an
unknown result to playout recovery. Stable payload backing outlives all queued,
active and completion references. Never rebind merely because generation is zero.

Inbound: four fixed outstanding owned JSON records, nonwaiting admission, source
and protocol/connect fencing, receipt and unchanged lesson transport epoch. Copy
borrowed cJSON while valid; never retain raw callback pointers. Preserve current
payload behavior without inventing an inbound byte cap (transport currently lacks
one); allocation/full failure explicitly faults the exact source, no silent drop
or unbounded Schedule fallback. Measure memory cost and fail allocation safely.

Deadline clarification: inbound receipt+10s bounds dispatch/start admission, not
the duration of an already-started lesson storage sync. Preserve existing long-tool
timeouts and cancellation behavior. The immutable source/lesson epoch and bounded
outstanding permit survive the continuation. A produced reply receives its own
original send-admission time and immutable10s send deadline, never refreshed on
Busy/rebind. Test a current-source tool completing after10s and stale-source
continuation cancellation with permit reclamation; do not time out valid lesson
sync merely to reuse a chat send deadline.

Unpair clarification: the ACK is a produced reply under this rule. Its original
10s send-admission deadline is fixed when BeginChatUnpair produces/adopts the ACK
obligation and also bounds delayed teardown. Inbound receipt+10s still governs
dispatch admission. Allocation failures, Busy and retries never renew that ACK
deadline; no extra inbound-anchored teardown exception is introduced.

## Task 1: Complete Routes And Activation

Files and responsibilities:

- `main/chat_outbound_mailbox.h`, `main/chat_outbound_worker.h/.cc`: full-text job
  reference and conditional dispatch without new packet or worker queue.
- New `main/chat_connection_messages.h`: two logical full-text records and exact
  physical-job retirement/outcome bookkeeping.
- New `main/chat_inbound_messages.h`: four owned JSON handoff slots and immutable
  request context; no UI/tool execution while mailbox lock is held.
- `main/protocols/protocol.h`, `main/protocols/websocket_protocol.h/.cc`: complete
  conditional-text API, construction-time callbacks and gated capability.
- `main/application.h/.cc`: sole dispatch, all message routes, selected lifecycle
  and actual initialization, response START effects and eligibility.
- `main/mcp_server.h/.cc`: immutable request/reply context through all synchronous,
  scheduled and storage-worker paths; no global current-source variable.
- Existing native/pytest fixtures and focused new
  `tests/test_chat_source_activation.py`, `tests/test_chat_connection_messages.py`,
  `tests/native/chat_connection_messages_test.cc` as needed for real adapters.

- [ ] Write failing full-text lifecycle tests and run
  `python3 -m pytest tests/test_chat_connection_messages.py -q` before code.
  Exercise actual mailbox/worker/conditional transport with immutable full text:

```text
queue full-text -> START retires generation -> exact reservation retires
assert no completion means untaken -> same-source physical rebind -> one send
hold taken send -> START -> return Sent -> assert logical terminal, no replay
hold send -> source replacement -> assert no send to successor, lifetime retained
simulate Failed after partial submit -> assert no automatic retry
hold completion/mailbox -> repeated START -> preserve payload and original deadline
```

- [ ] Implement two logical records and full-text conditional send using the
  existing sender. Keep response controls prioritized without starving mandatory
  cleanup. Test >128-byte, tools/list-size and65535-boundary complete envelopes,
  escapes, capacity, allocation failure, ASan/TSan storage lifetime.
- [ ] Write failing actual source-dispatch tests, then implement full routing for
  tts start/stop/sentence_start, stt, llm, mcp, system, alert, robot_action, custom
  configuration branch and lesson_ branch. Malformed/unknown/quiet/lesson behavior
  remains explicit. Callback return destroys input; later dispatch must stay valid.
  Lesson epoch passes unchanged, old source and old epoch cannot affect successor.
- [ ] Thread source-owned reply context through ParseMessage, DoToolCall,
  ReplyResult, ReplyError, GetToolsList, short scheduled tools, prepared calls and
  storage workers. Bound accepted outstanding work; no unbounded continuation
  captures bypassing inbound capacity. Reject stale source before tool effects
  and before reply. Preserve result content, pagination, IDs and error escaping.
  Same-source response replacement does not invalidate a connection-owned tool.
- [ ] System unpair ACK uses connection sender. Hold subsequent intentional
  teardown until actual ACK completion or original deadline; no immediate close
  after admission. Test success, Busy, failed send, timeout and lesson/quiet guards.
- [ ] Complete confirmed START effects once per admitted current generation:
  gesture BeginResponse, aborted=false, activity timestamp, legal Speaking state,
  speaking watchdog. Non-realtime input cleanup uses audio worker; qualified
  realtime capture is preserved. No stale START/JSON rendering or gesture effect.
- [ ] Audit and route selected network loss, close/error, Wi-Fi setup, repair,
  protocol reset and reboot through immediate invalidation and retained deferred
  cleanup. Preserve heartbeat/passive reconnect, lesson abandonment and setup
  screens. Check MaintainPassiveLiveness shared-socket work. Audit periodic metrics,
  queue/wake access, RearmClaimedIdleWakeWord, generic Alert/PlaySound and scheduled
  MCP/tool effects so selected control path has no hidden blocking audio/socket
  fallback. Do not move physical side effects to an unsafe execution context.
- [ ] Initialize actual audio cleanup plus outbound/protocol workers before
  eligibility. Install complete source callback set once before WebSocket Start/
  Open; MQTT stays legacy. Select source from actual validated open publication.
  Passive preconnection routes lesson/MCP with revoked mic; only valid explicit
  User/Wake promotes. Lesson transitions do not replace live std::functions.
  Capability advertisement requires successful complete initialization; failure
  fails closed with visible recovery, never a partially opted-in route.
- [ ] Run actual construction/hello/dispatch/lifecycle tests before enabling the
  source route and capability in the candidate. Tests must cover all routes above,
  allocation failures, passive/open/promote/lesson/reconnect, full-text blocking
  during START, network/decoder lock barriers, Wi-Fi/reboot state preservation and
  complete main event paths. Do not rely only on helper declarations/string scans.
- [x] Run focused sanitizer suite, then independent spec review and fixes, then
  independent quality review and fixes. Root runs full firmware tests and target
  build/hash/layout. Record increased task/static/dynamic memory and remaining
  target runtime headroom/scheduling uncertainty before any physical release.

## Release Boundary

Final local implementation verification passes after storage-owner and codec
eligibility corrections: root full1708passed2opt-in skips171.43s, expanded
68passed92.78s, spec and quality review PASS. Build0x3a3790/free0x4c870(8%);
app SHA c35037ee8200361facfcbab00cb44a0020ce9df7b365633a4439b2d2acdc9c43.
See docs/superpowers/reports/2026-09-09-chat-source-activation-verification.md
for exact commands, layout, memory, fixture limits and warnings. This supersedes
the earlier open review findings, not their historical evidence. Scoped Git
binding, clean server inventory/assembly, matched release and attended hardware
acceptance remain outstanding. No commit, deployment, serial or flash performed.

Storage-owner correction now passes full implementation spec re-review: TaskBody
returns before Entry self-deletes; regression checks one completion owner at
deletion, then permit expiry, and exits without unwinding the task stack. Both
normal/failsafe reviewer cases pass2tests3.05s. Root corrected expanded suite
68passed133.21s, target build0x3a3730/free0x4c8d0(8%), SHA
c0a2cbae52448754f72302b3d8654716db76ab28ab6933a84c2ccda38eeec3b3.
Root ELF sizes unchanged: Application6312, inbound76/envelope80, connection544,
mailbox832, LessonQueueItem616, StaticTask_t352, StackType_t1. Four JSON permits
bound count, not bytes; two full-text payloads max131070bytes plus owner/string
overhead. Runtime heap/stack/scheduling still unmeasured. Post-fix full suite and
independent quality review ongoing; no activation acceptance/commit/flash yet.

Full spec review reopened the freeze: LessonAssetSyncTaskBody retains a local
ChatRequestContext across vTaskDeleteWithCaps, which does not unwind C++ owners.
Root verified both exit paths; the existing returning task-delete stub hid this
permit leak. Scoped correction and deletion-boundary regression are in progress.
Pre-correction root build passes:0x3a3750/free0x4c8b0(8%), SHA
5ada0730fd7da6cfddf954be7f675acd9fa14cc2635d128d80e5e4429bfd587d.
Root six-file expanded sanitizer subset58passed61.63s; not full acceptance or
evidence for the pending correction. Final full/spec/quality gates remain open.

Shutdown correction bounded re-review PASS: root2focusedpassed1.67s/reviewer
2passed1.48s. Four producers now lease admission before handle/effect access;
Stop joins producers and requires retired plus kernel-confirmed eSuspended before
deletion. Both reported races closed; no general post-destruction callbacklifetime
claim. Final integrated activeOpen test/full/sanitizer/build/review still pending.

Approved shutdown correction checkpoint: independent review found retired flag
before self-suspend is not a cross-core join, and producer admission was not
closed before draining/freeing queue storage. Required correction closes producer
admission and joins in-flight leases before drain, then waits for actual permanent
task suspension before delete/stack free. Target eTaskGetState checks currently
running cores under the kernel lock; INCLUDE_eTaskGetState=1. No resume path may
exist after retirement. Test paused producer and retired-but-still-running task;
do not substitute the earlier mock's retired flag for scheduler quiescence. These
fixes and integrated active-Open Busy proof remain pre-freeze requirements.

Latest unreviewed activation checkpoint: complete source callbacks/capability are
now wired locally, not deployed. Implementer reports1705passed2opt-in skips134.93s
and expanded65sanitizerpassed52.26s before cooperative lesson shutdown changes.
Subsequent build SHA9e2441d11dc6d6851fb49fc6783cd6b96133e716a756e8d528ef7382164b663a
matches root's fresh binary hash. Root's three shutdown/adoption/init checks pass
1.01s, but shutdown helper stubs actual worker/task retirement. Full post-change
rerun, integrated active-Open/early-greeting Busy proof and independent review
remain required. Historical OFF/incomplete checkpoint below is superseded only
as to local wiring, not acceptance or physical release status.

Partial checkpoint2026-09-09 remains NOT accepted/NOT activated. Connection lane,
owned inbound permits and partial MCP plumbing exist, but dispatch context/lesson
fencing, nested tool effects, session_id envelope, live-generation unsent Stale
retry, unpair continuation and lifecycle/initialization remain incomplete. First
full run14failed1669passed2skipped; adapter/contract locators were repaired without
dropping the original checks. Storage worker failsafe now holds in_flight through
queued quiet completion; allocation failure of that Schedule still lacks a bounded
cleanup/recovery obligation. Do not call retained fail-closed state a full fix.
Root expanded sanitizer34passed34.25s; target build0x399850/9%free,
SHA0790778c330280d03516dc5f09b915ec7291f17a94f759c219c7df217e05ecca.
Implementer full rerun1683passed2opt-in skips115.56s. Independent review and
completion of remaining task items are still required.

Implementation activation is local candidate code, not deployment. Exact server
image Python3.10.21 regression currently845 PASS, but dirty source is not bound to
a release Git identity; current pinned source/node inventory omits new chat drain.
Do not bypass release tooling or pretend the production-base image digest is the
new candidate image digest. Preserve agent-only keys/model/AEC/volume/Neweye/
gestures and existing lesson behavior. Matched review/release, rollback-preserving
targeted flash and attended physical acceptance remain required. Ask for fresh
motor-area safety before any boot/conversation test that can trigger gestures.
