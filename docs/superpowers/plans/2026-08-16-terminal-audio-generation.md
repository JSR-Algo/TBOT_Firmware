# Terminal Lesson Audio Generation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bound terminal lesson TTS quarantine to the exact speaking generation so a later normal conversation can resume the microphone.

**Architecture:** Replace the unbounded boolean with one atomic 64-bit token encoding `speaking_generation + 1`. Arm it at `lesson_stop`, capture the active response generation when a TTS stop arrives, consume the token once in the Application callback, suppress only an exact match, and let stale mismatches continue through normal TTS-stop handling.

**Tech Stack:** ESP-IDF/C++17, FreeRTOS Application task, pytest source-contract tests, campaign RED/GREEN gate, LCDWiki production build, USB flash and physical voice/lesson E2E.

---

## File Map

- Modify `tests/test_realtime_voice_state.py`: add RED coverage for encoded generation arming, capture, exact-match quarantine, stale fallthrough, and activation cleanup.
- Modify `main/application.h`: replace the boolean terminal latch with a single 64-bit atomic generation token.
- Modify `main/application.cc`: arm, capture, consume, compare, and log the terminal generation.
- Create `docs/qa/ad-hoc/2026-08-16-t54-terminal-audio-generation.md`: record RED/GREEN, focused suites, build, gate, merge, artifact and physical evidence.
- Create `/Users/manhhodinh/Documents/TBOT/lesson-prod/repros/t54-terminal-audio-generation.sh`: persistent optimization-safe RED/GREEN gate probe.
- Modify `/Users/manhhodinh/Documents/TBOT/LESSON_PRODUCTION_PLAN.md`: resolve `F-T54-50` only after main and physical post-lesson voice verification.
- Modify `/Users/manhhodinh/Documents/TBOT/robot/docs/qa/ad-hoc/2026-08-16-t54-e2e-live.md`: append post-lesson voice and CP-7 evidence.

### Task 1: Add The Generation-Bound RED Regression

**Files:**
- Modify: `tests/test_realtime_voice_state.py:1265`
- Create: `docs/qa/ad-hoc/2026-08-16-t54-terminal-audio-generation.md`

- [ ] **Step 1: Replace the boolean-only terminal test with the desired token contract**

Keep the existing test name and its ordering assertion, then require these source contracts:

```python
def test_lesson_terminal_stop_quarantines_only_matching_tts_generation():
    app_h = read("main/application.h")
    app_cc = read("main/application.cc")
    lesson_handler = read("main/lesson_handler.cc")

    assert "std::atomic<std::uint64_t> lesson_terminal_audio_generation_{0};" in app_h

    stop_branch = lesson_handler[
        lesson_handler.index('if (strcmp(type, "lesson_stop") == 0)') :
        lesson_handler.index('if (strcmp(type, "lesson_error") == 0)')
    ]
    assert stop_branch.index("BeginLessonTerminalAudioQuiet();") < stop_branch.index(
        "SetLessonRuntimeActive(false);"
    )

    begin = function_body(app_cc, "void Application::BeginLessonTerminalAudioQuiet")
    compact_begin = " ".join(begin.split())
    assert "lesson_terminal_audio_generation_.store(" in begin
    assert "static_cast<std::uint64_t>(speaking_generation_.load()) + 1" in compact_begin

    tts_stop = app_cc[
        app_cc.index('strcmp(state->valuestring, "stop") == 0') :
        app_cc.index('} else if (strcmp(state->valuestring, "sentence_start") == 0)')
    ]
    assert "const std::uint64_t stopped_audio_generation" in tts_stop
    assert "static_cast<std::uint64_t>(speaking_generation_.load()) + 1" in " ".join(tts_stop.split())
    scheduled = tts_stop[tts_stop.index("Schedule([this") :]
    assert "stopped_audio_generation" in scheduled[scheduled.index("Schedule([this"):scheduled.index("]()")]
    assert "lesson_terminal_audio_generation_.exchange(0)" in scheduled

    match_guard = scheduled[
        scheduled.index("if (terminal_audio_generation == stopped_audio_generation)") :
        scheduled.index("if (lesson_runtime_active_.load() && !lesson_interactive_turn)")
    ]
    assert "SetDeviceState(kDeviceStateIdle);" in match_guard
    assert "return;" in match_guard
    assert "terminal_audio_generation != 0" in match_guard
    assert "protocol_->SendStartListening" not in match_guard
    assert "audio_service_.EnableVoiceProcessing(true);" not in match_guard

    setter = function_body(app_cc, "void Application::SetLessonRuntimeActive")
    assert "lesson_terminal_audio_generation_.store(0);" in setter
```

If the exact lambda close text differs from `]()` in current source, slice the capture list between `Schedule([this` and the lambda body `{` while retaining the semantic assertion that `stopped_audio_generation` is captured by value.

- [ ] **Step 2: Run focused RED**

```bash
python3 -m pytest -q tests/test_realtime_voice_state.py::test_lesson_terminal_stop_quarantines_only_matching_tts_generation
```

Expected: FAIL because main still declares `lesson_terminal_audio_quiet_` and has no generation token.

- [ ] **Step 3: Write RED evidence**

Create `docs/qa/ad-hoc/2026-08-16-t54-terminal-audio-generation.md` with finding `F-T54-50`, introducing commit `40ea482d`, confirmed data flow, approved design link, exact RED command/failure, and explicit statement that no follow-up fix has been flashed.

- [ ] **Step 4: Commit RED only**

```bash
git add tests/test_realtime_voice_state.py docs/qa/ad-hoc/2026-08-16-t54-terminal-audio-generation.md
git commit -m "test(lesson): reproduce stale terminal audio quarantine"
```

### Task 2: Implement The Atomic Terminal Generation Token

**Files:**
- Modify: `main/application.h:213`
- Modify: `main/application.cc:3381`
- Modify: `main/application.cc:3902`
- Modify: `main/application.cc:3921`

- [ ] **Step 1: Replace the boolean latch declaration**

```cpp
std::atomic<std::uint64_t> lesson_terminal_audio_generation_{0};
```

Do not retain a separate terminal boolean.

- [ ] **Step 2: Arm and clear the encoded token**

Change lesson activation cleanup to:

```cpp
if (active) {
    lesson_terminal_audio_generation_.store(0);
}
```

Change `BeginLessonTerminalAudioQuiet()` to:

```cpp
void Application::BeginLessonTerminalAudioQuiet() {
    lesson_terminal_audio_generation_.store(
        static_cast<std::uint64_t>(speaking_generation_.load()) + 1);
}
```

- [ ] **Step 3: Capture the stopped response generation before stop-side increments**

In the TTS-stop receive branch, immediately after deriving stop metadata and before the `is_interrupt` branch can increment `speaking_generation_`, add:

```cpp
const std::uint64_t stopped_audio_generation =
    static_cast<std::uint64_t>(speaking_generation_.load()) + 1;
```

Capture `stopped_audio_generation` by value in the scheduled callback.

- [ ] **Step 4: Consume and compare the terminal token**

Replace the boolean guard with:

```cpp
const std::uint64_t terminal_audio_generation =
    lesson_terminal_audio_generation_.exchange(0);
if (terminal_audio_generation == stopped_audio_generation) {
    ESP_LOGI(TAG,
             "terminal lesson tts stop matched generation=%llu state=%d",
             static_cast<unsigned long long>(stopped_audio_generation),
             static_cast<int>(GetDeviceState()));
    lesson_idle_repaint_suppressed_.store(true);
    SetDeviceState(kDeviceStateIdle);
    return;
}
if (terminal_audio_generation != 0) {
    ESP_LOGI(TAG,
             "stale terminal lesson tts stop ignored terminal=%llu stopped=%llu",
             static_cast<unsigned long long>(terminal_audio_generation),
             static_cast<unsigned long long>(stopped_audio_generation));
}
```

The mismatch branch must fall through to the existing active-lesson and normal `continue_listening` paths.

- [ ] **Step 5: Run focused GREEN and voice suite**

```bash
python3 -m pytest -q tests/test_realtime_voice_state.py::test_lesson_terminal_stop_quarantines_only_matching_tts_generation
python3 -m pytest -q tests/test_realtime_voice_state.py tests/test_lesson_sd_sync_worker_contract.py
```

Expected: focused test passes; both complete files pass.

- [ ] **Step 6: Run syntax/build-scope checks and commit**

```bash
git diff --check
```

Run the ESP-IDF compile command for `main/application.cc` with `-fsyntax-only` if available from `build/compile_commands.json`; otherwise rely on Task 4's complete production build.

```bash
git add main/application.h main/application.cc
git commit -m "fix(lesson): bind terminal quiet to speaking generation"
```

### Task 3: Add Persistent Gate Evidence And Re-verify

**Files:**
- Create: `/Users/manhhodinh/Documents/TBOT/lesson-prod/repros/t54-terminal-audio-generation.sh`
- Modify: `docs/qa/ad-hoc/2026-08-16-t54-terminal-audio-generation.md`

- [ ] **Step 1: Create an optimization-safe inline Python repro**

The executable script must declare:

```bash
# repo: /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware
```

Its inline Python must define explicit `require(condition, message)` checks rather than `assert`, extract the three relevant Application functions/blocks, and require:

```text
std::atomic<std::uint64_t> lesson_terminal_audio_generation_{0};
static_cast<std::uint64_t>(speaking_generation_.load()) + 1
stopped_audio_generation
lesson_terminal_audio_generation_.exchange(0)
terminal_audio_generation == stopped_audio_generation
terminal_audio_generation != 0
lesson_terminal_audio_generation_.store(0)
```

It must print `t54 terminal audio generation contract: PASS`.

- [ ] **Step 2: Verify normal and optimized direct execution**

```bash
bash /Users/manhhodinh/Documents/TBOT/lesson-prod/repros/t54-terminal-audio-generation.sh
PYTHONOPTIMIZE=1 bash /Users/manhhodinh/Documents/TBOT/lesson-prod/repros/t54-terminal-audio-generation.sh
```

Expected: both pass on branch tip; base will fail explicitly.

- [ ] **Step 3: Run focused and related suites**

```bash
python3 -m pytest -q \
  tests/test_realtime_voice_state.py \
  tests/test_lesson_sd_sync_worker_contract.py \
  tests/test_lesson_dispatch_backward_compat.py
bash scripts/run_host_native_lesson_handler_test.sh
```

Expected: all pass.

- [ ] **Step 4: Build LCDWiki production branch artifact**

```bash
PATH="/usr/bin:$PATH" ./build-lcdwiki.sh --no-flash
python3 scripts/assert_lcdwiki_prod_config.py sdkconfig
```

Record the exact branch SHA, config result, artifact size and SHA-256. This branch artifact is verification-only.

- [ ] **Step 5: Run campaign RED/GREEN gate**

```bash
cd /Users/manhhodinh/Documents/TBOT
bash lesson-prod/scripts/gate.sh \
  t54-terminal-audio-generation \
  /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware \
  lesson-prod/t54-terminal-audio-generation
```

Expected: RED on main/base, GREEN on exact branch tip, `VERIFIED` in `GATE_LOG.md`.

- [ ] **Step 6: Update and commit evidence**

Record all commands/results, baseline missing-doxygen note without overclaim, build identity, gate row/hash, and keep status `IN_PROGRESS - merge/flash/live pending`.

```bash
git add docs/qa/ad-hoc/2026-08-16-t54-terminal-audio-generation.md
git commit -m "docs(qa): record terminal audio generation verification"
```

Re-run the gate after the evidence commit so it grades the exact merge tip.

### Task 4: Merge Follow-up And Rebuild Frozen Main Artifact

**Files:**
- Update only gate-generated campaign records and ignored build artifacts.

- [ ] **Step 1: Preflight and exact-tip re-gate**

Confirm canonical main and task worktree are clean, fetch remotes, rebase only if main advanced, rerun the focused tests and repro, then gate the exact tip again. Stop on conflicts or dirty trees.

- [ ] **Step 2: Merge through the campaign script**

```bash
cd /Users/manhhodinh/Documents/TBOT
bash lesson-prod/scripts/merge-task.sh \
  t54-terminal-audio-generation \
  /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware \
  lesson-prod/t54-terminal-audio-generation
```

Expected: non-squash follow-up merge on firmware main; no push yet.

- [ ] **Step 3: Verify main in isolation**

```bash
bash lesson-prod/scripts/verify-on-main.sh \
  /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware -- \
  python3 -m pytest -q tests/test_realtime_voice_state.py tests/test_lesson_sd_sync_worker_contract.py
```

Expected: both files pass against the explicit main ref.

- [ ] **Step 4: Retire the obsolete unflashed main artifact worktree**

Check `/Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware/.worktrees/t54-main-flash` is detached, clean, and still at old main `790ce667`. Record its obsolete SHA, then remove only that worktree through `git worktree remove`. Do not delete any tracked branch.

- [ ] **Step 5: Recreate and build the new frozen main artifact**

Create the same detached path at new firmware main. Confirm `sdkconfig.defaults.local` is present and unchanged, then:

```bash
PATH="/usr/bin:$PATH" ./build-lcdwiki.sh --no-flash
python3 scripts/assert_lcdwiki_prod_config.py sdkconfig
```

Immediately record main SHA, `sdkconfig` SHA, `dependencies.lock` SHA, artifact size, mtime and SHA-256. Do not build again before flash.

### Task 5: Flash And Prove Post-Lesson Conversation Continuity

**Files:**
- Modify: `docs/qa/ad-hoc/2026-08-16-t54-terminal-audio-generation.md`
- Modify: `/Users/manhhodinh/Documents/TBOT/robot/docs/qa/ad-hoc/2026-08-16-t54-e2e-live.md`
- Modify: `/Users/manhhodinh/Documents/TBOT/LESSON_PRODUCTION_PLAN.md`

- [ ] **Step 1: Resolve and attest the robot USB port**

List explicit serial ports and run a chip-id read if more than one candidate exists. Do not guess.

- [ ] **Step 2: Verify the frozen artifact immediately before flash**

Recompute SHA-256 and compare byte-for-byte with Task 4's recorded hash. Abort if it differs.

- [ ] **Step 3: Flash without rebuilding**

Use:

```bash
BUILD_DIR=/Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware/.worktrees/t54-main-flash/build \
PORT=<explicit-port> \
/Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware/.worktrees/t54-main-flash/scripts/flash_prod_new_robot.sh
```

Do not call `build-lcdwiki.sh <port>` because it deletes the frozen build.

- [ ] **Step 4: Run the full physical lesson path**

Assign without PIN, start from passive conversation listening, confirm SD quiet succeeds, render and complete the approved three-layer lesson, and perform the real mid-lesson power cycle required by CP-7.

- [ ] **Step 5: Prove post-lesson normal TTS resumes the microphone**

After terminal lesson completion, begin a normal conversation and capture:

```text
stale terminal lesson tts stop ignored
```

only if a stale mismatch is actually observed, plus the mandatory normal path:

```text
mic_loop_resumed ... reason=tts_stop_continue_listening
```

Confirm the child can speak a second normal turn without restarting or entering a new lesson. Absence of the stale log is acceptable when the terminal token was consumed by the matching stop; successful normal mic resume is required.

- [ ] **Step 6: Resolve finding and continue T5.4 closeout**

Mark `F-T54-50` RESOLVED only with RED/GREEN, main merge, frozen artifact, flash, and post-lesson mic-resume evidence. Keep T5.4 `IN_PROGRESS` until all remaining CP-7/T5.4 Ship requirements pass.

## Self-Review

- Spec coverage: exact generation arming, atomic consume, match suppression, mismatch fallthrough, activation cleanup, TDD, gate, merge, frozen-main artifact, non-rebuilding flash, power cycle and post-lesson voice proof are mapped.
- Placeholder scan: every code change and verification step contains concrete source or commands.
- Type consistency: the plan consistently uses `std::uint64_t`, existing `std::uint32_t speaking_generation_`, token zero as unarmed, and `speaking_generation + 1` as the encoded identity.
