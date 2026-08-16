# T5.4 Cinematic Display Ownership — 2026-08-17

## Scope

Physical renderer-v5 lesson playback left the normal conversation face above the
lesson framebuffer. This lane changes only firmware lesson display ownership; the
three-layer asset contract and ESP lesson sequencing remain unchanged.

## Reproduction

Capture:

```text
/Users/manhhodinh/Documents/TBOT/.codex_tmp/lesson-live-20260817T001200Z-v7-display-fix
assignmentId=a9dd036d-7c82-4513-91b4-c0fdf571260b
sessionId=32d1074c-ea9f-48bb-ba61-cf398112cb48
lesson=w02-feelings v7
```

Before the fix, renderer-v5 entered `session_mode=LESSON` and accepted cinematic
playback while the already-visible conversation face remained above the authored
lesson content. The v5 path called `SetLessonRuntimeActive(true)` but did not claim
the display mode used by the legacy lesson path.

Regression commit `3f09768` adds source-contract checks that fail unless accepted
cinematic start claims lesson display mode and accepted stop/cancel releases it.

## Fix

Commit `45a7f56` updates `main/lesson_handler.cc`:

- accepted cinematic start calls `SetLessonMode(true)` before playback;
- accepted cinematic stop/cancel clears lesson layers and calls `SetLessonMode(false)`;
- rejected or duplicate commands do not change display ownership.

## Verification

```text
focused face/cinematic contract: 42 passed
expanded firmware contract set: 282 passed, 2 baseline WebSocket failures covered by F-T54-42
firmware build: PASS
xiaozhi.elf SHA256: 34cf64b3e1420aed2f204dfe6d943f9a9e3061cdb5b5d09945bee37fa8490661
xiaozhi.bin SHA256: 22c722727f7b781dca8583f4d6b9f04d5ab760cbffb3c512cc263d921799d0b6
T0.4 gate t54-cinematic-display-ownership: RED on 2ff314a, GREEN on 45a7f56
```

The image was flashed to physical robot MAC `14:c1:9f:d1:ac:20`; flash hashes were
verified. The lesson then completed s1-s9 and the backend assignment became
`COMPLETED`.

Operator physical confirmation:

```text
đã hết che
có đủ 3 lớp và mặt đã trở lại
```

This confirms the static high-quality background, static teaching object, animated
robot video, no conversation-face obstruction during the lesson, and normal face
restoration after completion.

## Routed Finding

The subsequent mid-lesson power-cycle recovered the RUNNING assignment but exposed
an ESP-side cached-SD-sync ordering race (`F-T54-56`). It is tracked and fixed in the
separate ESP server lane; it is not hidden or addressed by this firmware change.
