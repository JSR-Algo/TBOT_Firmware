# Google Live playout, distortion and echo repair

Status: written specification approved by the user on 2026-09-08.
Scope: high-risk maintenance of US-004, current firmware checkout and existing
Google Live server branch. Preserve unrelated display/GIF edits and agent-only
credentials. No motor commands, NVS erase or buffer-capacity increases.

## Evidence and limits

- User reports audible stutter/distortion and intermittent self-answering.
- Lowering volume92 to65 reduces distortion but does not eliminate it.
- Board startup previously overwrote saved volume; that separate defect is
  repaired and volume65 persistence was verified on the robot.
- OutputData wall time reached749738us for a60ms packet. This includes driver
  work, scheduling and codec logging; it does not prove an I2S hardware fault.
- Firmware resumes listening after a2s drain timeout. Google Live conversational
  stops do not currently use the lesson-only drain acknowledgement handshake.
- Server base output_gain1.35 can clip int16 PCM. A synthetic sample25000
  saturates to32767. Effective per-agent gain and real PCM clipping are not yet
  measured, so clipping is a candidate cause, not an established acoustic cause.
- Firmware clamps requested100 to92. Report requested and applied volumes
  separately; do not label testing at92 as a true100-volume hardware test.

## Selected approach

Use generation-scoped device playout completion for conversational turn
finalization, plus separately measured audio-path repairs. Extending a fixed
timeout cannot establish speaker completion. Permanently lowering volume masks
symptoms. Replacing full-duplex conversation with permanent half-duplex would
change interruption behavior and is not part of this design.

## Playout completion contract

1. Negotiate conversational drain support before enabling the handshake.
   Reuse the existing drainId/ack envelope where compatible, without changing
   lesson ownership or accepting legacy lesson acknowledgements as chat acks.
2. Allocate a unique pending drain identity for each connection/response before
   sending its terminal stop. Model generation complete is not device playout
   complete. Keep the existing output/echo guard during the pending drain.
3. Firmware tracks queued decode, decode in progress, queued PCM, output in
   progress, and remaining I2S/DMA tail. Queue-empty alone is insufficient.
   Use a nonblocking completion state machine; do not wait seconds on the main
   task or while holding audio locks. Any tail estimate must be derived from
   actual sample rate and DMA configuration and validated on hardware.
4. Only the matching completed response can acknowledge its drain. Server then
   applies the bounded residual echo tail and resumes normal listening. Samples
   captured under suppression must not be replayed after the guard opens.
5. Preserve explicit interruption: cancellation invalidates pending drain and
   discards old output. Existing barge-in qualification remains active during
   normal speech; arbitrary microphone echo must not be accepted as an interrupt.
   Do not silently disable interruption to make the silence test pass.
6. Duplicate/stale acknowledgements, old sockets, new response starts and
   reconnects cannot release another response's guard. Cleanup is idempotent.
7. A10s drain deadline is a recovery limit, not a completion signal. On expiry,
   cancel the response and request output abort; never emit successful drain or
   automatically reopen cloud input over unfinished output. If output cannot
   become quiescent, close the affected conversation and expose a recoverable
   audio error rather than remain in an apparently listening state. No silent
   automatic loop that feeds residual output back into Google Live.

Compatibility: legacy peers remain explicitly outside the new E2E acceptance
claim. Stage matched firmware/server capability support; do not enable a server
requirement that waits for acknowledgements an old device cannot send.

## Distortion and stutter investigation/repair

- Measure codec write duration separately from logging and surrounding task
  scheduling; record decode/encode durations and queue/in-flight state using
  bounded counters. Export summaries outside critical audio sections; never log
  raw speech or credentials. Do not treat between-turn silence as an underrun.
- Verify effective output gain without exposing agent secrets; measure pre/post
  gain peak and clipped-sample counts. If added gain is clipping, repair that
  amplification path without introducing frame-to-frame loudness pumping.
- Compare identical audio with the suspect scheduling/logging path isolated.
  Change one measured cause at a time; no speculative priority, AEC, microphone,
  GIF or buffer tuning. If evidence identifies a display interaction, report it
  before editing files owned by the display task.
- Do not remove the current speaker ceiling or force100 during this repair.
  A true100 hardware test needs a separately justified safe ceiling change.

## Verification and release gates

Write failing tests before implementation. Cover normal/slow playout, in-flight
last decode, DMA tail, timeout recovery, lost/duplicate/stale ack, replacement
socket, new turn, cancellation and lesson/chat isolation. Exercise runtime logic,
not only source-string contracts. Test PCM gain with low/high/full-scale samples
and chunk boundaries. Run firmware native/sanitizer tests, focused regressions,
full target build and server tests using the deployed Python/dependency versions.

Deploy only a verified matched candidate with rollback artifacts. Preserve
Wi-Fi, pairing, assets, agent credentials and server model selection. Verify MAC
14:c1:9f:d1:ac:20 (the currently confirmed single connected robot), image hash,
boot health and actual applied volume. Older log identities do not identify a
second physical robot or authorize flashing a different target.

Attended E2E acceptance requires:

- Ten30s response turns, each followed by15s user silence: zero unsolicited
  self-replies, zero cut-off tails, and no audible stutter/distortion reported.
- Five interruption/recovery turns: real user interruption still works and stale
  output/acks cannot reopen the wrong turn.
- At least three comparable speech turns at applied65 and92; a request100 must
  report the existing92 cap. Stop a loudness test if distortion becomes severe.
- Slow/lost-drain and reconnect fault tests: no false completion, no mic-echo
  replay, bounded recovery, no watchdog/panic or permanent stuck conversation.
- Correlate acoustic results with timing counters and session events. Report
  input-to-first-output and final-output-to-listening latency; do not claim fast
  response from network-only or synthetic tests.

Any remaining audible failure keeps US-004 physical acceptance NOT PASS. Slow
startup wake readiness is a known separate regression gate, not fixed by this
handshake. No production-ready claim until all applicable hardware gates pass.

## Self-review

No unresolved placeholders. Normal drain preserves speech; explicit abort alone
may cut speech. Timeout never means successful completion. Full-duplex barge-in
is retained, legacy rollout is explicit, and acoustic proof is separate from
software proof. Next step after written-spec approval: implementation plan.
