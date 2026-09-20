# Voice demo JSON admission - 2026-09-11

## Observed failure

The attended operator reports server-unavailable after a few replies. After
flashing the previously verified START/STOP clock fix (ELF da898fc1), USB records
HI ESP at 10:20:26 and a source fault at 10:21:36 (UTC+7). Four completed server
audio turns have matching drain ACKs before the failure. The receiver then
reports `json_admission outcome=2 retries=26 wait_us_lo=253090`, with three queued
and four outstanding JSON contexts. An in-progress DispatchIncomingJson scope
finishes after 1,265,964 us. The robot closes its source and displays recovery;
the operator confirms the same error. This is not a missing drain ACK or a
server restart in this trial.

## Bounded demo correction

The operator previously authorized hiding other features to prioritize HI ESP,
smooth speech and repeating 100%/0% arm motion. Keep the existing demo switch
and discard conversation-only STT, LLM emotion, and TTS sentence-start
presentation frames before they enter the four-context control queue. This
temporarily hides captions. Keep the animated face and state indicators.

Increasing queue capacity would add memory pressure; increasing admission
deadlines would keep the receiver blocked behind presentation. Neither is
needed for this attended demo. A new display worker is outside this narrow fix.
START/STOP, audio, system/MCP/arm commands and lesson-routed frames keep their
existing handlers and deadlines. Disable CONFIG_TBOT_VOICE_DEMO to restore
normal presentation. No server code or configuration is changed.

## Execution and proof

- [x] Reproduce a presentation burst against a full control queue using actual
  MakeChatSourceCallbacks. Demo enabled fails before the fix; demo disabled
  passes its original admission path.
- [x] Add the bounded receiver filter. Test 300 presentation frames, lesson
  routing, real controls and unchanged control timeout, then actual START,
  speech generation, arm response start and drain ACK with ASan/UBSan.
- [x] Run the adjacent chat/arm/UART slice with CHAT_APPLICATION_TSAN=1:
  45 passed in 43.64 s, no skips. CI lifecycle native scripts and the workflow's
  Python contract selection: 140 passed in 1.38 s. Actual LCDWiki build passes.
- [ ] Complete an attended conversation trial and record remaining failures.

An initial new journey fixture allowed too few polls to retire Setup's prior
outbound generation; correcting the fixture to exercise bounded retirement
passes. No production behavior was changed for that fixture issue.

Candidate ELF: b97afed8a9548d65e07a4125706e8d12f8d0fc83a7eff6fa52345fc2d30ce17c.
App SHA256: 443ac450f205441c5ad750093e3a956230f1add90b8d41c94462d132f3504b9c.
Size: 3,825,296 bytes. Exact dirty-source hashes, backup, test/build logs and
serial/server evidence are in parent workspace
`task-artifacts/voice-20260911-6x0_du05/`.

The prior unrelated lesson-production display-ownership repro still has its
documented baseline failure; this is not a full CI or 30-minute voice PASS.
Host adapters do not establish actual acoustic quality or servo motion.

## Caption and status follow-up

The operator reports good conversational flow but missing captions and a
Listening label during speech. The selected-chat rearm renderer handles Pending,
Armed and terminal phases, but omitted None during confirmed speech. A native
START journey reproduces the stale Listening label, including a render between
admission and confirmation. It now renders Speaking only after that state is
current, without reconfiguring audio or restarting a GIF.

Demo captions now use one bounded latest-message slot (767 UTF-8 bytes, no
split characters), with try-lock publication/consumption. The existing LVGL
task consumes it every 100 ms; no new task or larger control queue is added.
At consumption, connection/protocol/response ownership and active conversation
state are checked. Obsolete, cancelled or lesson-routed text is discarded.
LLM emotion bursts still bypass the control queue. LCD timer creation failure
logs a warning and leaves speech operational.

Final focused command:

```sh
CHAT_APPLICATION_TSAN=1 python3 -m pytest -q tests/test_chat_caption_mailbox.py tests/test_chat_demo_presentation.py tests/test_chat_stop_poll_race.py tests/test_chat_json_admission.py tests/test_chat_playout_intake.py tests/test_chat_start_handoff.py tests/test_chat_start_presentation.py tests/test_chat_listen_keepalive.py tests/test_speaking_arm_gesture.py tests/test_robot_uart_integration.py
```

49 passed in 51.40 s, including ASan/UBSan and TSan. Coverage includes a stalled
UI with 300 arriving captions, source/response revocation, UTF-8 boundaries,
concurrent publication/consumption, status transitions, and unchanged control
admission/START/drain ACK behavior. The four workflow native scripts and 140
Python contracts pass (1.32 s). ESP-IDF 5.5.4 LCDWiki build passes. The required
lesson-production suite still stops at its pre-existing
`t54-cinematic-display-ownership.sh` substring lookup failure.

Display candidate ELF:
`3ed1058268f6bf588f693218dad41566020e7ffef62b8b7e3b11f65bf8571604`.
App SHA256:
`236446d574761377d951f1d7f10b0ae2a6a61bd986a676db56aed31ebbd0a81b`.
App size 3,826,352 bytes. Source/config hashes are in `display-candidate.json`.
App-only flash at 0x20000 and separate application/protected-low digest checks
pass; USB boot confirms ELF `3ed105826`. Attended display/audio acceptance is
pending.

The preceding candidate's extended USB log also records recovery site 203 at
10:52:26 and later transport failures, including DNS failures around 11:04.
Those are retained as unresolved voice evidence; the operator's positive voice
feedback does not establish uninterrupted 30-minute operation. No server change
is included in this display correction.
