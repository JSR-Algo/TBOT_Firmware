# Realtime Voice E2E Stability Design

## Goal

Make the production robot reliably complete the full voice loop from the `Hi ESP`
wake phrase through a spoken response and return to idle, with fast first audio and
without WebSocket stalls, resets, or progressive memory degradation.

## Scope

This work is limited to firmware behavior on the LCDWiki ESP32-S3 robot and the
existing production voice protocol. Server behavior is observed for timing and
fault localization, but server contract changes are out of scope unless evidence
proves the firmware cannot satisfy the existing contract.

## Acceptance Criteria

A controlled physical run must complete ten consecutive voice turns:

1. The operator says `Hi ESP` and then a short Vietnamese question.
2. At least nine of ten wake attempts enter listening on the first utterance.
3. Every accepted wake sends microphone audio and receives playable TTS.
4. First response audio starts within 2,000 ms of the end of the operator's
   question for at least nine of ten completed turns; all turns must report their
   measured latency.
5. Every completed turn returns to idle with wake detection armed.
6. The run has no panic, reboot, stuck listening state, unexpected WebSocket drop,
   failed reconnect, audio queue growth, or heartbeat-worker allocation failure.
7. A fifteen-minute post-turn soak keeps the WebSocket and wake detector alive.

## Evidence Collection

One timestamped run directory records:

- raw firmware serial output;
- ESP server logs for the target device when available;
- a normalized turn timeline containing wake, listen start, first uplink packet,
  first TTS frame, playback start, TTS stop, and idle rearm;
- per-turn latency and pass/fail results;
- heap, largest internal block, task stack watermarks, queue depths, reconnects,
  drops, and reset markers throughout the run;
- firmware image hashes and the production endpoint configuration used for flash.

## Debugging And Remediation

Failures are handled one root cause at a time. Each component boundary is traced
from wake detection through audio capture, Opus, WebSocket, incoming TTS, playback,
and idle rearm. A firmware change requires a failing regression test or host-native
reproduction before implementation, followed by focused tests, a production build,
an NVS-preserving flash, and a fresh physical rerun.

The first investigation targets current internal-SRAM pressure because the latest
boot reached a 715-byte lifetime minimum and an HTTPS asset fetch failed to allocate
a 4,096-byte internal block. The fix must preserve the internal-stack safety of
TLS/NVS workers while restoring enough contiguous headroom for voice reconnects and
background asset traffic.

## Safety And Rollback

Flashing writes bootloader, partition table, OTA data, application, and generated
assets only. It does not erase or write NVS. Every candidate is checked for the
LCDWiki board, production OTA/WebSocket URLs, and disabled local-endpoint mode.
If a candidate regresses boot, connectivity, wake detection, or memory safety, the
last verified production image is reflashed with the same NVS-preserving layout.

