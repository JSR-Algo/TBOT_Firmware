# Speaking arm gestures

Status: approved by user; implementation and physical validation pending.

## Scope

During normal conversational replies, automatically move the arms lightly.
Keep neweye display assets and existing voice behavior. No automatic head
movement, lesson gestures, server/model changes, or physical motion testing
without an attended safe setup. Explicit user arm commands take priority.

## Options

1. Firmware playback-driven gestures (recommended): local timing, no additional
   cloud request, cancellation tied to the same response as playback.
2. Server-issued gestures: additional transport timing and cancellation coupling.
3. Model-selected gestures: less deterministic timing and extra tool calls.

## Proposed behavior

Use a bounded gesture controller and a separate UART worker; audio callbacks and
the application task never wait for UART writes or servant acknowledgements.
Start only after actual reply playback begins, not when the model is merely
thinking. Updated by the user's 2026-09-10 demo request: alternate left 100%,
left 0%, right 100%, right 0% of existing calibrated travel, at most one new
target per second. Do not change global servo speed.
Validate whether the existing servant can make this trajectory smoothly before
making any physical smoothness claim.

Tag work with the current response identity. Stop scheduling and discard stale
work on playback completion, interruption, disconnect, state departure, lesson
entry or an explicit arm command. Explicit commands retain ownership until the
next conversational response. Never enqueue an automatic return-to-rest that
could overwrite a user's command. Already accepted servo movement cannot be
claimed physically stopped without a supported servant cancel/hold command.

Use a bounded latest-target mailbox rather than an accumulating motion queue.
If UART is unavailable or overloaded, skip the gesture without delaying audio.
No movement on boot or wake alone. Never replay old gestures after reconnection.

## Verification

Write failing native tests before implementation for playback start, cadence,
travel limits, response replacement, interruption, explicit-command priority,
lesson exclusion and unavailable UART. Test worker cancellation and bounded
backpressure using controlled effects; retain existing motion/audio regressions.
Build the actual ESP32-S3 target and verify neweye asset mapping separately.

Before flash, identify the attached board, confirm it is the intended target,
back up the actual app/assets and verify partition offsets. Preserve NVS,
pairing and Wi-Fi. Physical E2E requires the screen connected, robot stable,
clear arm travel and an operator present. Flash success is not motor or acoustic
acceptance. Existing Google Live audio-drain integration remains a separate
unfinished task and must not be presented as completed by this feature.
