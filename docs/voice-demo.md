# Voice And Arm Demo

The attended demo uses the existing Hi ESP wake model, Google Live conversation,
and playback-driven arm gestures on the LCDWiki ES3C35P. No server deployment,
credentials, audio gain, echo policy, servo speed or servo limits are changed.

Enable `CONFIG_TBOT_VOICE_DEMO=y` in the existing board sdkconfig to defer lesson
SD downloads and ignore per-transcript emotion changes. The current animated
face, status and captions remain available. This option defaults off. Disable it
and rebuild to restore those lesson/expression features after the demo.

## Fixes In This Candidate

- Admit a newly arrived TTS START between JSON presentation operations, retaining
  the original 250 ms admission deadline and source checks.
- Render a relistening phase once per playback-reset token and offline status,
  rather than restarting its GIF on every audio readiness poll.
- Block the AFE fetch task for one actual tick between fetches. On the board's
  100 Hz FreeRTOS configuration, the previous 1 ms conversion produced zero
  ticks and could starve lower-priority capture preparation.
- Ignore Google Live's no-audio listen refresh only when the microphone is
  already authorized and listening, with no receiver-owned START. This existing
  server envelope has no drain ID; treating it as a malformed audio STOP caused
  recovery site 203 after a successfully drained reply. Active speech, invalid
  IDs, interrupts and stale-source checks keep their existing behavior.
- Refresh speaking activity when the current response's audio packet is accepted.
  The selected chat path omitted this update, so the 12-second inactivity timer
  could abort a progressing long answer. Rejected/obsolete packets do not refresh
  it, and an actual inactive response still times out.
- Route selected-chat radio loss through the existing source-failure recovery.
  The intentional-close path cancels online intent and was preventing reconnection
  after Wi-Fi returned. Duplicate radio/socket failures are handled once; user
  stop and abort still cancel recovery.
- Retire a delivered drain ACK immediately, including while the audio snapshot
  is busy. Otherwise a later stop, abort or silence timeout can retain that old
  ACK as active and fail at control deadline site 216 after relistening.
- Restore wake detection when stop, silence expiry or non-resuming abort returns
  to claimed idle. The selected chat state path bypasses legacy idle effects;
  use the same claimed/not-connecting/not-syncing eligibility in its cleanup.
- In demo mode, expose the servant's servo ACK and PONG at info log level to
  distinguish UART writes from servant receipt. ACK is not physical motion proof.

## Attended Check

1. Keep the robot on a stable surface with its arms clear. Connect normal power
   and Wi-Fi. Wait for the wake detector to start after boot.
2. Say "Hi ESP", wait for the greeting, then ask a short question.
3. Check intelligible audio, alternating arm movement during actual speech, and
   return to listening. Repeat with a longer answer and a wake interruption.
4. Continue conversation for 30 minutes. Record stalls, retries, echo, arm
   behavior and serial/server events. Boot, tests, UART sends and idle uptime
   alone do not establish an uninterrupted 30-minute conversation.

Evidence for the 2026-09-10 trial is under workspace
`task-artifacts/robot-demo-2eunfezh/`. Raw serial/server logs and the NVS-containing
rollback backup are private. Firmware is written only to active ota0 at
`0x20000`; keep its previous image for rollback. Do not use a full-flash script
that overwrites NVS, OTA metadata or assets for this demo.
