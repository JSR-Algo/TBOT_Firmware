# Passive Listening SD Sync Quiet Design

**Status:** Approved direction 1 for implementation

**Goal:** Allow an assigned lesson to attest or materialize its exact SD pack
when the robot is passively listening, without permitting storage mutation
during real child speech, robot speech, connection setup, reset, or an active
lesson.

## Context

T5.4 live testing proved that the server and firmware disagree about the
pre-lesson state. The ESP server treats realtime `LISTENING` as download-safe
when none of its transient audio signals are active. Firmware
`BeginLessonAssetSyncQuiet()` currently accepts only `kDeviceStateIdle`.
Consequently, a valid foreground `self.lesson_assets.sync_to_sd` request reaches
the robot while it is passively listening and is rejected before the first
lesson render:

```text
lesson asset sync quiet rejected state=5 lesson=0 connect=0 reset=0
lesson asset sync busy or worker unavailable
SD_SYNC_REALTIME_BUSY_TIMEOUT state=LISTENING
```

The cached pack, cache key, checksum, authentication, and backend assignment are
already proven valid by the definitive renderer-v5 capture. This change only
aligns quiet admission with the existing passive-listening safety contract.

## Chosen Design

`BeginLessonAssetSyncQuiet()` continues to atomically acquire
`lesson_asset_sync_quiet_` before changing audio state. Once acquired, it admits
exactly two device states:

- `kDeviceStateIdle`; or
- `kDeviceStateListening` only when `IsVoiceDetected()` is false.

All existing fail-closed guards remain mandatory: no lesson runtime, connection
attempt, reset, storage session, storage mutation, or second sync may be active.
Speaking, connecting, unknown, and every other device state remain rejected.

For admitted passive listening, the Application task performs the existing
listening shutdown sequence before the worker is created:

1. suppress the idle repaint used by conversation UI;
2. send `listen/stop` on the current protocol when available;
3. clear listening activity timestamps;
4. disable voice processing and wake-word detection;
5. transition the device state to `kDeviceStateIdle`;
6. reset the decoder and drain queued outbound audio;
7. return success so the existing single-flight SD worker may start.

The quiet flag is acquired first, so wake, start-listening, reconnect, TTS, and
STT paths cannot reopen voice ownership during the transition. The worker keeps
the existing `LessonAssetStorageCoordinator` mutation lease and exact-pack
checksum verification; this design does not create a read-only bypass or weaken
storage exclusion.

## Completion And Recovery

`EndLessonAssetSyncQuiet()` remains the only normal release boundary. It resets
passive WebSocket liveness and re-enables wake-word detection only when the
device is still safely idle, claimed, audio is running, and no lesson,
connection, or reset owns the device.

If quiet admission fails, the flag is cleared and no listening state, audio
queue, storage, or lesson state is changed. If the worker cannot be allocated or
created, the existing cleanup schedules `EndLessonAssetSyncQuiet()` before
returning the MCP error. Worker success and failure retain the same cleanup and
response ordering.

## Safety Boundaries

- Child speech wins: `IsVoiceDetected()` rejects a passive-listening takeover.
- Robot speech remains rejected because `kDeviceStateSpeaking` is not admitted.
- An active lesson remains rejected by both Application and storage coordinator
  guards.
- The MCP request still performs the full exact cache-key, path, file size, and
  SHA-256 validation; no recent-attestation shortcut is introduced.
- No new MCP tool, WebSocket message, backend contract, persistent flag, or
  production database mutation is added.
- Conversation face rendering is unchanged; lesson ownership still begins only
  after SD attestation succeeds.

## Verification

Implementation follows RED-GREEN TDD.

Firmware contract regressions must prove:

- passive `LISTENING` with no detected voice is converted to idle quiet before
  worker creation;
- detected voice while `LISTENING` is rejected without state mutation;
- speaking, connecting, active lesson, reset, and concurrent sync remain
  rejected;
- the quiet flag gates new voice transitions throughout admission and sync;
- worker allocation, worker creation, tool failure, and outer failsafe paths
  release quiet exactly once;
- existing mutation/session exclusion and sync attestation tests remain green.

After host and firmware suites pass, build and flash the production artifact,
then run the physical T5.4 path: assign without PIN, start from conversation
listening, confirm first render, complete the lesson, and perform a real
mid-lesson power cycle. The renderer-v5 evidence verifier, server live probe,
CP-7 sidecars, merge/deploy rules, verify-on-main, and worktree cleanup remain
required before T5.4 can be marked DONE.

## Alternatives Rejected

### Server-side prepare-quiet command

Adding a new server-to-firmware lifecycle command would duplicate the
Application state's authority, expand the wire contract, require coordinated
server deployment, and still need firmware-side race protection before the SD
worker starts.

### Recent-attestation sync bypass

Skipping the MCP sync for a recent checksum cannot prove SD state after cold
boot, power loss, card replacement, or incomplete prior mutation. It does not
satisfy the required mid-lesson power-cycle recovery evidence.

## Rollback

Rollback restores idle-only quiet admission. It changes no stored lesson pack,
assignment, NVS record, backend row, or SD cache format. The observable rollback
effect is that lesson start from passive `LISTENING` again fails closed before
SD sync.
