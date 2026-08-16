# Terminal Lesson Audio Generation Design

**Status:** Approved direction 2 for implementation

**Goal:** Quarantine only the late TTS stop that belongs to the terminal lesson
response, without allowing terminal quiet state to suppress a later normal
conversation response.

## Context

T5.4 introduced `lesson_terminal_audio_quiet_` to prevent a late terminal
`tts state=stop` frame from reopening realtime listening after `lesson_stop`.
The ordering protection is necessary, but the boolean is an unbounded latch:

- `lesson_stop` sets it before deactivating the lesson runtime;
- the TTS-stop handler checks it before normal `continue_listening` handling;
- runtime deactivation does not clear it; and
- only the next lesson activation clears it.

Therefore a later non-lesson TTS stop can be forced to IDLE instead of reopening
the microphone. Wake/manual input can recover, so this is not a permanent audio
deadlock, but it violates post-lesson conversation continuity. The finding is
tracked as `F-T54-50` in `LESSON_PRODUCTION_PLAN.md`.

The existing terminal quarantine was introduced by `40ea482d` and merged into
firmware main at `790ce667`. No firmware from that main commit has been pushed or
flashed for this closeout.

## Chosen Design

Replace the boolean terminal latch with one atomic generation token:

```cpp
std::atomic<std::uint64_t> lesson_terminal_audio_generation_{0};
```

Token zero means no terminal quarantine is armed. A nonzero token encodes the
current 32-bit `speaking_generation_` plus one, so speaking generation zero is
representable without colliding with the unarmed sentinel.

`BeginLessonTerminalAudioQuiet()` snapshots and publishes:

```cpp
lesson_terminal_audio_generation_.store(
    static_cast<std::uint64_t>(speaking_generation_.load()) + 1);
```

The TTS-stop receive path snapshots the same encoded generation before posting
its Application-task callback. The callback consumes the terminal token exactly
once with `exchange(0)`:

```cpp
const std::uint64_t terminal_generation =
    lesson_terminal_audio_generation_.exchange(0);
if (terminal_generation == stopped_audio_generation) {
    // This is the late stop for the terminal lesson response.
    lesson_idle_repaint_suppressed_.store(true);
    SetDeviceState(kDeviceStateIdle);
    return;
}
```

If the consumed token is zero, normal TTS-stop handling continues unchanged. If
it is nonzero but does not equal the stopped response generation, the token is
stale: it is consumed and normal `continue_listening` handling proceeds. The
mismatch is logged with numeric generations only; it does not expose lesson or
child identity.

`SetLessonRuntimeActive(true)` continues to clear terminal quarantine, now by
storing token zero. Runtime deactivation does not clear it because the exact late
terminal stop can arrive after deactivation.

## Ordering And Race Behavior

The token is a single sequentially consistent atomic, so validity and identity
cannot disagree as they could with separate boolean and generation atomics.

- If `lesson_stop` arms before the TTS stop is received, both snapshots match and
  the terminal stop is quarantined.
- If `lesson_stop` arms after receive but before the scheduled callback, the
  captured stopped generation still matches and is quarantined.
- If the TTS stop finishes before `lesson_stop` arms, that stop is not
  quarantined. The armed token remains for the next stop, whose newer generation
  mismatches; the callback consumes the stale token and continues normally.
- Duplicate or later stop callbacks cannot repeatedly suppress listening because
  the first callback consumes the token with `exchange(0)`.
- A 32-bit speaking-generation wrap is encoded losslessly in 64 bits; zero remains
  reserved solely for the unarmed state.

## Safety Boundaries

- Preserve the existing ordering: terminal quarantine is armed before lesson
  runtime deactivation.
- Preserve the active-lesson non-answer-turn guard below the terminal guard.
- Do not change TTS playback drain, interrupt flushing, lesson interactive turns,
  WebSocket contracts, renderer behavior, SD sync, or server code.
- Do not clear quarantine merely because the runtime becomes inactive; that
  recreates the original late-terminal-stop race.
- Do not use a permanent boolean plus a separately updated generation; the
  single atomic token is the race boundary.

## Verification

Implementation follows RED-GREEN TDD in a follow-up firmware branch from main.

Regression coverage must prove:

- `lesson_stop` arms the encoded current speaking generation before runtime
  deactivation;
- TTS stop captures its response generation before scheduling;
- an exact generation match consumes the token, forces IDLE, and does not reopen
  voice processing;
- a stale generation mismatch consumes the token and falls through to the normal
  `continue_listening` path;
- token zero leaves normal TTS-stop behavior unchanged;
- lesson activation clears the token;
- the existing late-terminal TTS quarantine assertions remain green;
- realtime voice, lesson handler, and production ESP-IDF build gates remain green.

After merge, rebuild the preserved firmware-main artifact and record its exact
main SHA, config hashes, size, mtime, and SHA-256. Flash that frozen binary with
`scripts/flash_prod_new_robot.sh` and an explicit `BUILD_DIR`; do not invoke
`build-lcdwiki.sh <port>` because it deletes and rebuilds the verified artifact.

## Alternatives Rejected

### One-shot boolean consumption

Using `lesson_terminal_audio_quiet_.exchange(false)` bounds the latch, but it
cannot distinguish the intended late terminal stop from a newer response when
`lesson_stop` and TTS stop arrive out of order.

### Clear on wake/manual listening

Clearing at explicit voice-intent entry points improves recovery but leaves a
normal post-lesson TTS response vulnerable until such an intent is observed and
requires multiple entry points to stay synchronized.

## Rollback

Rollback restores the boolean latch implementation. It changes no persisted
state, lesson assignment, SD pack, backend row, or wire message. Rollback also
restores the known `F-T54-50` post-lesson mic-resume risk and therefore is not a
release-ready state.
