# Passive Listening SD Sync Quiet Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the production robot start an assigned lesson from passive conversation listening by safely transitioning to SD-sync quiet, while still rejecting real child speech and every unsafe storage or lifecycle state.

**Architecture:** Keep the firmware Application task as the single authority for the audio-to-storage handoff. `BeginLessonAssetSyncQuiet()` atomically owns the quiet flag, classifies idle versus passive listening, converts only voice-silent listening to idle, and then lets the existing single-flight MCP worker perform unchanged exact-pack validation and storage mutation. No ESP-server, backend, mobile, wire-contract, or database change is required.

**Tech Stack:** ESP-IDF/C++17 firmware, FreeRTOS Application task, pytest source-contract regressions, ESP-IDF production build, USB flash, physical renderer-v5 lesson E2E.

---

## File Map

- Modify `tests/test_lesson_sd_sync_worker_contract.py`: pin passive-listening admission, detected-voice rejection, transition ordering, and existing fail-closed guards.
- Modify `main/application.cc`: implement the minimal passive-listening-to-idle quiet transition.
- Create `/Users/manhhodinh/Documents/TBOT/lesson-prod/repros/t54-passive-sync-quiet.sh`: self-contained RED/GREEN gate probe that does not depend on the task branch surviving cleanup.
- Modify `docs/qa/ad-hoc/2026-08-16-t54-passive-sync-quiet.md`: record RED, GREEN, build, flash, physical run, gate, merge, and verify-on-main evidence.
- Modify `/Users/manhhodinh/Documents/TBOT/robot/docs/qa/ad-hoc/2026-08-16-t54-e2e-live.md`: append the physical T5.4 session and CP-7 verdicts.
- Modify `/Users/manhhodinh/Documents/TBOT/lesson-prod/t54-e2e-live.md`: append final evidence and set DONE only after every Ship item passes.
- Modify `/Users/manhhodinh/Documents/TBOT/LESSON_PRODUCTION_PLAN.md`: resolve `F-T54-49`, append any newly discovered out-of-scope finding, and set T5.4 DONE only at final closeout.

### Task 1: Add The Failing Firmware Contract Regression

**Files:**
- Modify: `tests/test_lesson_sd_sync_worker_contract.py:101`

- [ ] **Step 1: Replace the idle-only assertion with the desired admission contract**

Add this test next to `test_sync_worker_owns_application_audio_quiet_lifecycle()`:

```python
def test_sync_quiet_admits_only_idle_or_voice_silent_passive_listening():
    source = read("main/application.cc")
    begin = function_body(source, "bool Application::BeginLessonAssetSyncQuiet")

    assert "const DeviceState state = GetDeviceState();" in begin
    assert "const bool passive_listening" in begin
    assert "state == kDeviceStateListening" in begin
    assert "!IsVoiceDetected()" in begin
    assert "state != kDeviceStateIdle && !passive_listening" in begin

    passive = begin[
        begin.index("if (passive_listening)") :
        begin.index("tts_audio_accepting_.store(false)")
    ]
    required = (
        "lesson_idle_repaint_suppressed_.store(true)",
        "protocol_->SendStopListening()",
        "listening_started_ms_.store(0)",
        "last_listening_activity_ms_.store(0)",
        "audio_service_.EnableVoiceProcessing(false)",
        "audio_service_.EnableWakeWordDetection(false)",
        "SetDeviceState(kDeviceStateIdle)",
    )
    for statement in required:
        assert statement in passive
    assert passive.index("protocol_->SendStopListening()") < passive.index(
        "SetDeviceState(kDeviceStateIdle)"
    )
```

Update the existing guard loop so it continues to pin:

```python
for guard in (
    "lesson_runtime_active_.load()",
    "connect_in_flight_.load()",
    "reset_pending_.load()",
):
    assert guard in begin
```

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```bash
pytest -q tests/test_lesson_sd_sync_worker_contract.py::test_sync_quiet_admits_only_idle_or_voice_silent_passive_listening
```

Expected: FAIL because the current function contains neither `passive_listening` nor `IsVoiceDetected()` and still rejects every non-idle state.

- [ ] **Step 3: Record RED evidence**

Create `docs/qa/ad-hoc/2026-08-16-t54-passive-sync-quiet.md` with the live failure signature and focused pytest failure. Do not claim the fix or CP-7 yet.

- [ ] **Step 4: Commit the failing regression and RED evidence**

```bash
git add tests/test_lesson_sd_sync_worker_contract.py docs/qa/ad-hoc/2026-08-16-t54-passive-sync-quiet.md
git commit -m "test(lesson): reproduce passive listening SD sync refusal"
```

### Task 2: Implement The Minimal Application Handoff

**Files:**
- Modify: `main/application.cc:3948`

- [ ] **Step 1: Classify safe passive listening after acquiring quiet ownership**

Replace the idle-only state guard in `BeginLessonAssetSyncQuiet()` with:

```cpp
    const DeviceState state = GetDeviceState();
    const bool passive_listening =
        state == kDeviceStateListening && !IsVoiceDetected();
    if ((state != kDeviceStateIdle && !passive_listening) ||
        lesson_runtime_active_.load() ||
        connect_in_flight_.load() ||
        reset_pending_.load()) {
        lesson_asset_sync_quiet_.store(false);
        ESP_LOGW(TAG,
                 "lesson asset sync quiet rejected state=%d voice=%d lesson=%d connect=%d reset=%d",
                 static_cast<int>(state),
                 IsVoiceDetected() ? 1 : 0,
                 lesson_runtime_active_.load() ? 1 : 0,
                 connect_in_flight_.load() ? 1 : 0,
                 reset_pending_.load() ? 1 : 0);
        return false;
    }
```

The log may report the second instantaneous voice sample for diagnostics, but admission is decided only by the first `passive_listening` snapshot.

- [ ] **Step 2: Convert admitted passive listening to idle before decoder reset**

Insert before the existing `tts_audio_accepting_.store(false)` line:

```cpp
    if (passive_listening) {
        lesson_idle_repaint_suppressed_.store(true);
        if (protocol_) {
            protocol_->SendStopListening();
        }
        listening_started_ms_.store(0);
        last_listening_activity_ms_.store(0);
        audio_service_.EnableVoiceProcessing(false);
        audio_service_.EnableWakeWordDetection(false);
        SetDeviceState(kDeviceStateIdle);
    }
```

Do not modify `EndLessonAssetSyncQuiet()`, the MCP worker, storage coordinator, sync attestation, or any server code.

- [ ] **Step 3: Run the focused regression and verify GREEN**

```bash
pytest -q tests/test_lesson_sd_sync_worker_contract.py::test_sync_quiet_admits_only_idle_or_voice_silent_passive_listening
```

Expected: `1 passed`.

- [ ] **Step 4: Run the complete SD-sync quiet contract file**

```bash
pytest -q tests/test_lesson_sd_sync_worker_contract.py
```

Expected: all tests pass; worker single-flight, cleanup, liveness, reconnect, and audio gates remain pinned.

- [ ] **Step 5: Inspect the diff for scope and whitespace**

```bash
git diff --check
git diff -- main/application.cc tests/test_lesson_sd_sync_worker_contract.py
```

Expected: only the approved Application admission and regression test differ.

- [ ] **Step 6: Commit the implementation**

```bash
git add main/application.cc
git commit -m "fix(lesson): enter SD sync quiet from passive listening"
```

### Task 3: Add The Persistent RED-GREEN Gate Probe

**Files:**
- Create: `/Users/manhhodinh/Documents/TBOT/lesson-prod/repros/t54-passive-sync-quiet.sh`

- [ ] **Step 1: Create a self-contained source-contract probe**

The script must declare the firmware repo and carry its own inline Python probe so it fails on the merge base and passes on the branch tip:

```bash
#!/usr/bin/env bash
set -euo pipefail
# repo: /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware

python3 - <<'PY'
from pathlib import Path


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[brace:index + 1]
    raise AssertionError("unterminated function")


source = Path("main/application.cc").read_text(encoding="utf-8")
body = function_body(source, "bool Application::BeginLessonAssetSyncQuiet")
compact = " ".join(body.split())
assert "state == kDeviceStateListening && !IsVoiceDetected()" in compact
assert "state != kDeviceStateIdle && !passive_listening" in compact
passive = body[body.index("if (passive_listening)"):body.index("tts_audio_accepting_.store(false)")]
assert "protocol_->SendStopListening()" in passive
assert "SetDeviceState(kDeviceStateIdle)" in passive
assert passive.index("protocol_->SendStopListening()") < passive.index("SetDeviceState(kDeviceStateIdle)")
print("t54 passive sync quiet contract: PASS")
PY
```

Use `apply_patch`, not shell redirection, when creating the real script. Mark it executable with `chmod +x`.

- [ ] **Step 2: Run the repro against the branch tip**

```bash
/Users/manhhodinh/Documents/TBOT/lesson-prod/repros/t54-passive-sync-quiet.sh
```

Expected: `t54 passive sync quiet contract: PASS`; `git status --short` remains unchanged.

- [ ] **Step 3: Run the campaign gate RED to GREEN**

```bash
cd /Users/manhhodinh/Documents/TBOT
bash lesson-prod/scripts/gate.sh \
  t54-passive-sync-quiet \
  /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware \
  lesson-prod/t54-face-motion-guard
```

Expected: base is RED, branch tip is GREEN, and `lesson-prod/GATE_LOG.md` records `VERIFIED`.

### Task 4: Re-verify The Firmware Branch And Build Production

**Files:**
- Modify: `docs/qa/ad-hoc/2026-08-16-t54-passive-sync-quiet.md`

- [ ] **Step 1: Run focused lesson/audio/storage regressions**

```bash
pytest -q \
  tests/test_lesson_sd_sync_worker_contract.py \
  tests/test_lesson_sd_sync_attestation_contract.py \
  tests/test_lesson_sd_sync_no_claim_gate_contract.py \
  tests/test_lesson_sd_sync_watchdog_contract.py \
  tests/test_realtime_voice_state.py \
  tests/test_lesson_passive_websocket_contract.py
```

Expected: all selected tests pass.

- [ ] **Step 2: Run the existing native sync and attestation suites**

```bash
bash scripts/run_host_native_lesson_asset_sync_path_test.sh
bash scripts/run_host_native_lesson_asset_sync_attestation_test.sh
```

Expected: both host-native binaries exit 0.

- [ ] **Step 3: Run the firmware standard pytest suite**

```bash
pytest -q
```

Expected: suite passes, or any unrelated baseline failure is reproduced on clean `main` and routed to the findings log without being fixed in this lane.

- [ ] **Step 4: Build the exact LCDWiki production configuration**

```bash
./build-lcdwiki.sh --no-flash
python3 scripts/assert_lcdwiki_prod_config.py sdkconfig
```

Expected: board hard gate prints `OK: LCDWiki ES3C35P`, build completes, `xiaozhi.bin` exists, and the config auditor passes.

- [ ] **Step 5: Record branch-tip verification evidence**

Append exact commands, exit codes, pass counts, binary size, config result, commit SHA, and `git diff --check` result to `docs/qa/ad-hoc/2026-08-16-t54-passive-sync-quiet.md`.

- [ ] **Step 6: Commit the completed branch evidence**

```bash
git add docs/qa/ad-hoc/2026-08-16-t54-passive-sync-quiet.md
git commit -m "docs(qa): record passive listening SD sync verification"
```

### Task 5: Merge Through The Gate And Verify Main

**Files:**
- Update generated campaign evidence only through the documented scripts.

- [ ] **Step 1: Confirm clean branch and current main ancestry**

```bash
git status --short
git fetch --all --prune
git rebase main
```

Expected: worktree is clean before rebase; resolve no unrelated changes. Re-run Task 4 focused tests and the repro after a successful rebase.

- [ ] **Step 2: Re-run the RED-GREEN gate at the rebased tip**

```bash
cd /Users/manhhodinh/Documents/TBOT
bash lesson-prod/scripts/gate.sh \
  t54-passive-sync-quiet \
  /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware \
  lesson-prod/t54-face-motion-guard
```

Expected: `VERIFIED` at the exact tip to be merged.

- [ ] **Step 3: Merge with the campaign merge script**

```bash
cd /Users/manhhodinh/Documents/TBOT
bash lesson-prod/scripts/merge-task.sh \
  t54-passive-sync-quiet \
  /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware \
  lesson-prod/t54-face-motion-guard
```

Expected: a non-squash merge lands on firmware `main`; do not push unless the campaign protocol explicitly requires it.

- [ ] **Step 4: Verify the focused contract on an isolated main worktree**

```bash
cd /Users/manhhodinh/Documents/TBOT
bash lesson-prod/scripts/verify-on-main.sh \
  /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware -- \
  pytest -q tests/test_lesson_sd_sync_worker_contract.py
```

Expected: all tests pass against `main`, not the shared checkout's current HEAD.

- [ ] **Step 5: Build the deployable artifact from isolated main**

Use a dedicated temporary worktree of firmware `main`, run `./build-lcdwiki.sh --no-flash`, and preserve the resulting build path until USB flashing completes. Record the exact main SHA and artifact SHA-256; never flash a branch-only artifact after merge.

### Task 6: Flash And Run Physical T5.4 Evidence

**Files:**
- Modify: `docs/qa/ad-hoc/2026-08-16-t54-passive-sync-quiet.md`
- Modify: `/Users/manhhodinh/Documents/TBOT/robot/docs/qa/ad-hoc/2026-08-16-t54-e2e-live.md`
- Write captures under: `/Users/manhhodinh/Documents/TBOT/robot/docs/evidence/t54-live-20260816-final/`

- [ ] **Step 1: Resolve the exact USB port without guessing**

```bash
ls -1 /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.wchusb* /dev/cu.SLAB_USBtoUART* 2>/dev/null
```

Expected: one explicit robot serial port. If multiple ports exist, identify the robot by an attended chip-id read before flashing.

- [ ] **Step 2: Flash the production artifact built from firmware main**

Run the checked-out main worktree's `build-lcdwiki.sh <explicit-port>` or `scripts/flash_prod_new_robot.sh` with `BUILD_DIR` set to the preserved main build. Expected: chip identity succeeds, flash completes, and the robot reboots.

- [ ] **Step 3: Confirm reboot and production connectivity**

Capture serial evidence for network connected, passive lesson WebSocket opened, SD pack sync completed or exact-pack attestation succeeded, and no reset/OOM/storage-busy signature.

- [ ] **Step 4: Assign the renderer-v5 lesson without PIN**

Use the existing authenticated mobile assignment path. Record assignment, session, child, cache key, manifest checksum, and firmware main SHA. Do not mutate production DB rows or READY packs directly.

- [ ] **Step 5: Start from conversation listening and prove first render**

Speak the start-lesson intent while the robot is in conversation `LISTENING`. Required evidence:

```text
lesson asset sync quiet begin
lesson asset sync quiet end
```

and no occurrence of:

```text
lesson asset sync quiet rejected state=5
lesson asset sync busy or worker unavailable
SD_SYNC_REALTIME_BUSY_TIMEOUT
```

Confirm the conversation face does not cover the lesson, the background and object remain high-quality static images, and the robot layer plays the approved fly-in/walk/listen/teach/thinking/celebrate/exit video effects.

- [ ] **Step 6: Perform the real mid-lesson power cycle**

After at least one rendered interactive phase, physically remove and restore robot power. Record the operator timestamp, serial boot evidence, assignment/session recovery, exact SD-pack attestation, resumed lesson frame, and final completion. Logs alone cannot substitute for the physical action.

- [ ] **Step 7: Complete CP-7 sidecars and live probes**

Run:

```bash
cd /Users/manhhodinh/Documents/TBOT
bash robot/scripts/tbot_live_e2e_probe.sh
```

Run the renderer-v5 definitive capture verifier against `/Users/manhhodinh/Documents/TBOT/robot/docs/evidence/t54-live-20260816-final/definitive-capture`; expected `101/101`. Capture Android progress after completion and confirm the completed lesson appears for the active child.

- [ ] **Step 8: Route any out-of-scope finding**

Append a dated row under `LESSON_PRODUCTION_PLAN.md` section 5 with severity, owning task, evidence, and status. Do not expand this firmware fix to unrelated server, backend, mobile, Wi-Fi-loss, asset-authoring, or database changes.

### Task 7: Close Evidence, Status, And Worktree Cleanup

**Files:**
- Modify: `docs/qa/ad-hoc/2026-08-16-t54-passive-sync-quiet.md`
- Modify: `/Users/manhhodinh/Documents/TBOT/robot/docs/qa/ad-hoc/2026-08-16-t54-e2e-live.md`
- Modify: `/Users/manhhodinh/Documents/TBOT/lesson-prod/t54-e2e-live.md`
- Modify: `/Users/manhhodinh/Documents/TBOT/LESSON_PRODUCTION_PLAN.md`

- [ ] **Step 1: Write final evidence without overstating CP-7**

Record exact SHAs, gate verdict, verify-on-main output, artifact checksum, flash port/result, assignment/session/child IDs, serial excerpts, physical power-cycle operator confirmation, renderer verifier result, live probe result, and Android progress screenshot. If any item is absent, keep T5.4 `IN_PROGRESS`.

- [ ] **Step 2: Resolve the routed blocker only when proven**

Change `F-T54-49` to RESOLVED with links to the RED/GREEN regression and physical start evidence. Leave unrelated findings unchanged.

- [ ] **Step 3: Confirm merge ancestry and worktree cleanliness**

```bash
git status --short
git merge-base --is-ancestor lesson-prod/t54-face-motion-guard main
```

Expected: no output from status and ancestry exit 0.

- [ ] **Step 4: Remove the merged firmware worktree and branch**

From the canonical firmware checkout:

```bash
git worktree remove /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware/.worktrees/t54-face-motion-guard
git branch -d lesson-prod/t54-face-motion-guard
```

Delete a remote branch only if one exists and campaign policy requires it. Stop rather than remove anything if the branch is unmerged or the worktree is dirty.

- [ ] **Step 5: Mark T5.4 DONE last**

Only after all prior steps pass, set DONE with evidence links in `/Users/manhhodinh/Documents/TBOT/lesson-prod/t54-e2e-live.md` and `/Users/manhhodinh/Documents/TBOT/LESSON_PRODUCTION_PLAN.md` section 2. Confirm the firmware worktree no longer appears in `git worktree list`.

## Self-Review

- Spec coverage: passive listening admission, child-speech rejection, unchanged mutation safety, cleanup, host verification, production build, physical first render, real power cycle, CP-7, merge, verify-on-main, evidence, and cleanup are each mapped to a task.
- Placeholder scan: every implementation and verification step contains concrete code or commands.
- Type consistency: the plan uses existing `DeviceState`, `kDeviceStateListening`, `IsVoiceDetected()`, `lesson_asset_sync_quiet_`, `protocol_`, and audio/timestamp members exactly as declared in `Application`.
