# Chat Recovery Continuation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Preserve one bounded user recovery intention through fault cleanup without replaying stale controls or cancelling deliberate shutdown.

**Architecture:** Application retains recovery ownership; the existing network worker opens the replacement connection and the existing audio worker resolves wake text. Recovery polling never opens the microphone itself. Presentation and server behavior remain unchanged in this first slice.

**Tech Stack:** ESP-IDF 5.5, ESP32-S3 LCDWiki, C++17 native adapters, pytest, ASan/UBSan/TSan.

User explicitly selects direct existing main checkout; do not create another
worktree or commit the broad pre-existing dirty application. Save the exact
before/after evidence so review excludes unrelated edits.

## Task 1: Retained Recovery And Actual Application Integration

Files: modify `main/application.cc`, `main/application.h`; create
`main/chat_recovery_intent.h` only if needed to isolate portable policy;
create `tests/test_chat_recovery.py` and native fixtures under
`tests/native/`; adapt existing Application test adapters only for new real
dependencies. Do not replace the lifecycle methods under test with mocks.

- [x] Save pre-edit application.cc/application.h and hashes to a private task artifact.
- [x] Write RED regression using extracted actual HandleReconnectTick and
  lifecycle methods. Hold a real ProtocolWorkLifetime reservation, request a
  fault close, fire the one-shot callback, release cleanup, and drive the
  retained notification/timer until an actual fresh-source adoption occurs.
  Required assertions:

```cpp
assert(!app.microphone_uplink_authorized_);
assert(!opened_while_cleanup_owned);
assert(replacement_open_count == 1);
assert(successor_source_adopted);
```

- [x] Run `python3 -m pytest -q tests/test_chat_recovery.py` and record
  behavioral assertion failures, not compiler/setup errors.
- [x] Add one Application-owned bounded intent (background/wake/listen,
  protocol/connect identity, original start/deadline and mode). A duplicate
  explicit event preserves the original deadline; a newly promoted background
  intent receives its first explicit 10s budget. Overflow/backward time expires
  fail-closed. Do not store old wire jobs or retain borrowed JSON.
- [x] Integrate fault-close capture before cleanup, deferred timer handling,
  missing-source wake/listen, worker retirement, source adoption and explicit
  cancellation. Only an identified recovery open may update the intent's
  connect generation. Reuse existing open/audio workers. Publish diagnostics
  with fixed numeric outcomes, no private content.
- [x] Add RED cases one at a time for duplicate wake, stale source/completion,
  original deadline, worker admission/open failure and every terminal path in
  the approved spec. Verify explicit wake produces fresh Wake before ListenStart
  exactly once; no user audio before delivery and preparation gates.
- [x] Implement the minimal change per RED case; run the focused command after
  each change. Do not loosen the failure assertions or increase timeout/queues.
- [x] Run ASan/UBSan and supported TSan focused proof. Report actual control,
  cleanup and source-boundary coverage separately from stubbed peripherals.
- [x] Self-review and hand the exact diff plus RED/GREEN outputs to root. No
  broad application commit, flash, deploy, asset, audio or motion edits.

## Task 2: Independent Review And Candidate Verification

Files: task evidence, same recovery files only for reviewed corrections.

- [x] Spec reviewer compares every ownership/cancellation/verification clause
  with actual code and runs selected regressions; fix and re-review all gaps.
- [x] Quality reviewer checks exact task diff, scheduling, identity, allocation,
  test fidelity and lifecycle variants after spec review passes.
- [x] Root reruns focused recovery, lifecycle, source activation, control/ping,
  START, JSON and playout suites. Then run `python3 -m pytest -q tests`.
- [x] Build using the configured ESP-IDF environment and existing sdkconfig;
  verify `build/xiaozhi.bin` with esptool image_info, record SHA256, sizes and
  toolchain/config identity. Preserve all unchanged assets.
- [x] Update this plan and parent validation/test matrix with exact results,
  skips, limitations and binary identity. No hardware PASS inferred from host.

## Task 3: Authorized App-Only Hardware Trial

Files: private serial/server/acoustic logs and release receipt; no server patch.

- [x] Inventory the sole USB device and verify no other serial owner. Revalidate
  MAC14:c1:9f:d1:ac:20, active partition metadata and rollback before writing.
- [x] Flash only the verified active app slot (expected0x20000); independent
  verify_flash must match. Never generic idf.py flash/erase or overwrite assets.
- [x] Capture boot/source-adoption logs, autonomously emit a bounded Mac wake
  and only emit a question after the reply/relisten gate. Restore Mac settings.
- [ ] Test fault/recovery only through an existing scoped robot-session
  mechanism; if none exists, report that physical fault-injection gap. Do not
  restart the shared VPS service merely to manufacture a disconnect.
- [x] Report wake/listen/first audio/drain/recovery and actual acoustic evidence.
  Stop at recurrent START/JSON failure and return to the approved presentation
  stage rather than claim this slice solves all speech issues.

## Plan Review

Spec clauses map to Task1 ownership/lifecycle regressions, Task2 review/build,
and Task3 physical gates. Presentation/echo remain separate measured slices.
No independent production writes run in parallel. User approval of the written
spec is the execution instruction; no further workflow-choice gate is needed.

Task3 latest: coordinated handover, target/rollback checks, app-only flash and
separate app/protectedlow/full-assets verification complete. Three captures
yield4synthetic wakes,1detection,1initial listening,0completeQ/A; E2EFAIL.
Question followed initial local listening, not a verified provider greeting.
Passive fault/recovery adoption observed but no intentional fault injection or
post-recovery spoken PASS. Source-marker delay11.32s and AFEbackpressure need
boundary diagnosis before any new behavior change. See
task-artifacts/recovery-flash-yPUFEW/RESULT.md. Prior hardware hold below is
historical and has been resolved; firmware remains installed and running.

## Execution Checkpoint

Historical first candidate: initial implementation frozen at application SHAa219b3103f8a;
27recovery cases and42final fixture/ping/recovery cases pass. Independent spec
review found a P1: explicit expiry during a stalled open changes to Idle,
then watchdog does not schedule background backoff. Candidate is NOT approved
for flash. Target build317835f2b834 passes (0x3a5bd0,7%free); this build is
pre-review-correction evidence only. Root full suite with all three supported
TSan switches was run before allowing review corrections. Exact evidence
and fixture limitations: task-artifacts/chat-recovery-K1ZdN5/RECOVERY-HANDOFF.md.

Final software checkpoint: application98a3a7eef458, appb950cddba7b5,
3824880bytes/7%free. Both review findings corrected and independently re-reviewed;
spec+qualityPASS. Root fullsuite/all3TSan switches1814PASS293.38s/no skips;
target build/imagevalidationPASS. Neweye/config unchanged. Task3 is held because
another active course-mode task used the same robot/MAC; awaiting user priority.
Revalidate current partitions/rollback after coordinated handover. No flash or
deploy in this implementation. Physical communication remains FAIL; next
presentation/START and echo gates are not solved by this recovery slice.
