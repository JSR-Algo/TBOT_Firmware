# T5.4 Terminal Audio Generation Follow-up

**Date:** 2026-08-16
**Status:** IN_PROGRESS - branch verified; merge, flash, and live proof pending

## Finding

`F-T54-50` is a post-lesson voice-continuity regression introduced by
`40ea482d` and merged into local firmware main at `790ce667`. The terminal
lesson TTS guard is stored as an unbounded boolean. `lesson_stop` arms it before
runtime deactivation, but normal runtime deactivation does not clear it and the
TTS-stop handler checks it before `continue_listening`. A later normal response
can therefore be forced to IDLE instead of reopening the microphone.

The confirmed data flow is:

```text
lesson_stop
  -> BeginLessonTerminalAudioQuiet()
  -> SetLessonRuntimeActive(false)
  -> later normal tts stop
  -> stale terminal guard returns before mic_loop_resumed
```

Wake/manual listening can recover, so this is not a permanent audio deadlock.
The approved correction binds quarantine to the exact speaking generation:
[Terminal Lesson Audio Generation Design](../../superpowers/specs/2026-08-16-terminal-audio-generation-design.md).

## RED Regression

Command:

```bash
python3 -m pytest -q tests/test_realtime_voice_state.py::test_lesson_terminal_stop_quarantines_only_matching_tts_generation
```

Observed pre-fix result: `1 failed in 0.23s` at the first generation-token
assertion:

```text
AssertionError: assert 'std::atomic<std::uint64_t> lesson_terminal_audio_generation_{0};' in app_h
```

Firmware main still declares `lesson_terminal_audio_quiet_` and does not provide
a generation token, stopped-response capture, atomic consume, or
stale-generation fallthrough.

No follow-up production fix, merge, flash, post-lesson mic-resume evidence, or
CP-7 completion is claimed in this RED section.

## Fix

Commits:

- `a903468` adds the generation-bound RED source contract and this evidence file.
- `a6a36cd` replaces the boolean latch with
  `std::atomic<std::uint64_t> lesson_terminal_audio_generation_{0}`.

`BeginLessonTerminalAudioQuiet()` stores the current speaking generation plus
one, the TTS-stop receive path captures that generation before any interrupt
increment, and the scheduled callback consumes the terminal token with
`exchange(0)`. Only an exact generation match is quarantined. A nonzero stale
mismatch is logged and falls through to the existing normal
`continue_listening` path. Starting a later lesson clears any remaining token.

## GREEN Verification

Focused test:

```bash
python3 -m pytest -q \
  tests/test_realtime_voice_state.py::test_lesson_terminal_stop_quarantines_only_matching_tts_generation
```

Result: `1 passed`.

Voice and SD quiet suites:

```bash
python3 -m pytest -q \
  tests/test_realtime_voice_state.py \
  tests/test_lesson_sd_sync_worker_contract.py
```

Result: `158 passed`.

Related lesson suites, re-run at branch commit `a6a36cd`:

```bash
python3 -m pytest -q \
  tests/test_realtime_voice_state.py \
  tests/test_lesson_sd_sync_worker_contract.py \
  tests/test_lesson_dispatch_backward_compat.py
```

Result: `203 passed in 0.34s`.

Native lesson handler:

```bash
bash scripts/run_host_native_lesson_handler_test.sh
```

Result: `lesson host test OK (2624 checks)`.

`git diff --check` also exited 0. The repository-wide pytest collection has an
existing, clean-main-matched host dependency gap because `doxygen` is absent;
the focused project suites and native test above are the applicable green
checks for this follow-up.

## Persistent Gate Probe

Probe:

```text
/Users/manhhodinh/Documents/TBOT/lesson-prod/repros/t54-terminal-audio-generation.sh
```

Commands from the firmware worktree:

```bash
bash /Users/manhhodinh/Documents/TBOT/lesson-prod/repros/t54-terminal-audio-generation.sh
PYTHONOPTIMIZE=1 \
  bash /Users/manhhodinh/Documents/TBOT/lesson-prod/repros/t54-terminal-audio-generation.sh
```

Both print `t54 terminal audio generation contract: PASS`. The probe uses
explicit checks rather than Python `assert`, so optimized execution verifies
the same contract.

## Branch Production Build

Exact branch SHA:

```text
a6a36cdaf398314fc8be1914485bcfbc1786306c
```

Commands:

```bash
PATH="/usr/bin:$PATH" ./build-lcdwiki.sh --no-flash
python3 scripts/assert_lcdwiki_prod_config.py sdkconfig
```

Result: build exit 0 and `LCDWiki production build config OK`.

Artifact identity:

```text
path: build/xiaozhi.bin
size: 3776064 bytes
mtime: 2026-08-16T16:50:50+0700
sha256: cc6204ea2744539aa52c05f5601b7485699ba0659a433a5922f4445f16c08dbb
sdkconfig sha256: 2cb4d535ed0e243938cf6350ce917d1242816bbf7a3f1b7131c4a45c14a27ad7
dependencies.lock sha256: 74b2b4c8d734a59153ff0cfadd23a6a09558c7cd6e62f7a32829d88e485b0880
```

This branch artifact is verification-only and must not be flashed. The flash
artifact must be rebuilt once from the merged, frozen firmware `main` tip.

## Campaign Gate

Command:

```bash
cd /Users/manhhodinh/Documents/TBOT
bash lesson-prod/scripts/gate.sh \
  t54-terminal-audio-generation \
  /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware \
  lesson-prod/t54-terminal-audio-generation
```

Initial result before committing this evidence update:

```text
GATE PASS: t54-terminal-audio-generation VERIFIED
RED@base 790ce6671c0715973cfdaf2a53ee061c0e35c31f rc=1
GREEN@tip a6a36cdaf398314fc8be1914485bcfbc1786306c rc=0
```

The same command was then re-run after the evidence commit, with the branch
`HEAD` as the GREEN tip. It again returned `GATE PASS` with `RED@base rc=1` and
`GREEN@tip rc=0`; the immutable exact SHA is recorded by the generated campaign
`GATE_LOG.md` row rather than embedded recursively into its own commit.

Merge, frozen-main artifact creation, flash, post-lesson microphone proof, and
CP-7 remain pending and are not claimed here.
