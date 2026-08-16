# T5.4 Terminal Audio Generation Follow-up

**Date:** 2026-08-16
**Status:** IN_PROGRESS - RED captured; no follow-up firmware has been flashed

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
