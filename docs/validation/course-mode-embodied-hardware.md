# Course Mode V2 Embodied Hardware Validation

## Evidence status

- Firmware repository: `TBOT-Firmware`
- Task branch: `course-mode/task-03-firmware-embodied`
- Implementation base observed before Task 6 edits: `706a289866d444b7005a8d948f44199053bf8999`
- Final reviewed software candidate: `3a43ac12abd6bde053519e74d008a0fbd2a3a4db`
- LCDWiki no-flash build: `xiaozhi.bin` SHA-256
  `5be58d580346ab79ab7d2f90e854e52a7440f2233e465fa597feb1b1f55b6281`
- Generated assets SHA-256:
  `d03b074c39d78601b2a2f6c3438620adc1cf779d634825385e63cafc4528a52b`
- Observation date: 2026-08-22 (Asia/Ho_Chi_Minh)
- Serial device visible read-only: `/dev/cu.usbmodem1101`
- Flash, OTA, feature enablement, and production assignment: not performed
- Physical measurements: not executed; no microphone probe, current instrument, temperature
  probe, or attended servo observation was available to this software-only session
- Release status: **BLOCKED for production** until every required row below has measured,
  attributable evidence and all acceptance criteria pass

A visible serial device proves only that macOS exposes a port. It does not prove which
firmware image is running, that servos are powered, or that embodied motion is safe.

## Required setup

- Record final merged firmware SHA, build ID, board serial, board revision, servo model,
  power supply model/rating, and calibration/rest-pose reference.
- Use an attended test area with unobstructed arm/head travel and an immediate power cutoff.
- Capture microphone input while servos move and through the complete settle-before-listen
  interval; retain raw traces with timestamps correlated to wire frames and firmware logs.
- Measure supply voltage/current at idle, peak motion, restore, and settled listening.
- Measure servo case temperature before session 1 and after every fifth session.
- Do not begin a run if calibration, mechanical limits, supply stability, or emergency stop
  is uncertain.

## Acceptance criteria

- All 20 sessions return head to 50%, left arm to 0%, and right arm to 0% on completion,
  stop, pause, cancellation, and transport abandonment when hardware is responsive.
- Rest is reached within 2,000 ms of every teardown request.
- No servo command or continued mechanical movement occurs inside an assessed speech window.
- Lost ACK and duplicate delivery never replay physical motion.
- Reduced-motion mode sends zero servo commands and completes with the approved face/focus cue.
- Supply stays within the board/servo specification with no brownout or reset.
- Microphone motor-noise evidence is reviewed against the speech detector/noise budget.
- Servo temperature remains within the servo vendor's operating limit with no abnormal rise,
  odor, binding, chatter, or stalled current.

## Twenty-session measurement record

Fill every cell with measured values or linked artifact names. `PENDING` is deliberately used
here because no physical run was authorized or executed.

| Run | Journey / fault | Settle ms | Mic motor-noise artifact | Idle / peak current | Min supply V | Servo temp C (L/R/H) | Return pose | Stop/reconnect | Result |
|---:|---|---:|---|---|---:|---|---|---|---|
| 01 | Present left | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING |
| 02 | Present right | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING |
| 03 | Left/right supersession | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING |
| 04 | Lost terminal ACK + duplicate | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING |
| 05 | Exact cancel during hold | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING |
| 06 | Child barge-in/listen open | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING |
| 07 | Comfort calm | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING |
| 08 | Try different way | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING |
| 09 | Listen still | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING |
| 10 | Reduced motion | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING |
| 11 | Pause during active pose | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING |
| 12 | Stop during active pose | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING |
| 13 | Transport disconnect | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING |
| 14 | Reconnect and fresh action | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING |
| 15 | Firmware restart/power recovery | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING |
| 16 | One-servo response failure | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING |
| 17 | Face/display unavailable | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING |
| 18 | Settle timer failure injection | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING |
| 19 | Mastery celebration 1 and 2 | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING |
| 20 | Mastery celebration 3 energy cap | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING | PENDING |

## Per-run checklist

- Confirm exact assignment/session/step/action/generation and capture inbound frame.
- Confirm only a frozen named intent is accepted; capture terminal firmware ACK.
- Confirm ACK outcome is only `applied`, `degraded`, or `rejected`.
- Confirm `superseded` and `timed_out` never appear as firmware ACK outcomes.
- Correlate command, movement end, rest pose, settle completion, and listening-open timestamps.
- Record whether speech assessment was open and verify zero motion within that interval.
- Photograph or instrument the return pose; subjective visual memory is insufficient.
- Record abnormalities and stop immediately on binding, chatter, brownout, excess heat, or odor.

## Software evidence available before HIL

The host suite locks journeys 16-20, lifecycle teardown, exact ACK identity, lost-ACK
non-replay, assessment-window rejection, reduced motion, partial servo failure, face absence,
timer failure, same-session ledger restoration, and mastery capping. Host evidence cannot
substitute for physical latency, acoustic, electrical, thermal, or pose measurements.
