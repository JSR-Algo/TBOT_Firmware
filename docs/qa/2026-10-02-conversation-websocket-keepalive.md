# Conversation WebSocket keepalive repair — 2026-10-02

## Outcome

The firmware defect is fixed, built and flashed to the USB-connected LCDWiki ES3C35P (MAC 14:c1:9f:d1:ac:20). Full firmware suite: 1931 passed, no skips/xfails. Post-flash passive socket and management heartbeat remained healthy for over five minutes. The physical user wake trial at 15:44:31 failed because Google Live explicitly rejected the open with depleted prepaid credits (1011). Conversation acceptance is BLOCKED by the configured Google project billing; post-conversation silence remains unverified. Do not claim elimination of all upstream Google Live failures or production/fleet readiness.

## Cause and change

Application::Run required passive_ws_intent_ before maintaining JSON keepalive. Wake-word and user listening clear that flag while preserving online_intent_. The robot therefore stopped sending JSON pings during normal conversations and subsequent idle. The server deliberately disables protocol-level keepalive; captured old-image sessions without JSON pings repeatedly closed after about 128–130 seconds. The exact external transport component initiating the close was not established.

Application::Run now also maintains the existing source-fenced ping/pong path when a claimed normal-chat source is selected with online intent. Setup, audio-test, SD-sync, protocol-lifetime ownership and connect-in-flight guards remain. Lesson answer turns keep their existing behavior. A failed normal-chat probe feeds PollChatProtocolSignals / HandleChatSourceFailure rather than passive-lesson reconnect. No authentication, quotas, server keepalive policy or timeout thresholds changed.

Variant analysis: wake word, manual start and recovery all clear passive intent, so the common gate fixes their normal-conversation path. Legacy/MQTT and lesson-answer routes were excluded deliberately. Existing source-fencing, worker and recovery tests exercise stale callbacks, cancellation and reconnect; the new regression executes the actual clock gate, MaintainChatPassiveLiveness and PollChatConnectionMessages with host boundary fixtures, production liveness, ProtocolWorkLifetime, mailbox and logical delivery. It covers speaking, listening, post-chat idle, pending/other reservations, setup/sync, duplicate probe admission and active/passive timeout routing. Transport send and normal recovery effects are fixtures here; this is not radio/timing or full physical-journey proof.

## Verification

- Before fix: python3 -m pytest tests/test_conversation_websocket_liveness.py -q — 4 failed, 11 passed. Listening/speaking/idle lacked due probes; active timeout did not enter normal recovery.
- Final liveness regression: python3 -m pytest tests/test_conversation_websocket_liveness.py -q — 17 passed (ASan/UBSan). This run used the final strengthened fixture after the full suite began.
- Focused transport/worker/source/recovery/application suites — 101 passed.
- Final pipeline set: python3 -m pytest tests/test_conversation_websocket_liveness.py tests/test_chat_connection_messages.py tests/test_chat_outbound_worker.py tests/test_chat_recovery.py -q — 56 passed.
- Full suite: CHAT_MAILBOX_TSAN=1 TBOT_ESP_WORKTREE=/Users/manhhodinh/Documents/TBOT/robot/esp32-server python3 -m pytest tests/ -q -rs — 1931 passed in 336.47s, no skips or xfails.
- CHAT_MAILBOX_TSAN=1 python3 -m pytest tests/test_chat_outbound_mailbox.py -q -rs — 5 passed.
- ESP-IDF v5.5.4, SDK git 8e48797f, existing sdkconfig and default overlays: idf.py build passed, app size 0x3af5a0 (fits 0x3f0000).
- python3 scripts/assert_lcdwiki_prod_config.py sdkconfig passed; git diff --check passed. Existing SDK/Kconfig/shared_ptr deprecation warnings remain in the successful build.
- Independent code review found no confirmed defect; strengthened tests following its feedback to exercise real mailbox completion, logical delivery, ping-ID release and actual lifetime reservations.

Earlier full run: 2 failed, 1927 passed, 2 skipped. One test fixture assumed polling always had a fault even for a stale source; corrected the boundary stub and independently re-ran. The other used stale esp-cpr20260911 canonical-fixture mapping; corrected command points to actual esp32-server, without creating replacement contracts. The two skips were opt-in ThreadSanitizer tests; enabled and passed. All original logs retained.

## Device installation and evidence

Backed up the complete current ota_0 app partition (0x20000, size 0x3f0000) before mutation. Backup app ELF identity matches the diagnosed old image (20325b7b4afc7bcb5f52431b7bafe207fbb06702dbbb50a914e4fcd14cb8e536). Used esptool to write only the saved app binary at 0x20000; flash verified hash. No NVS erasure, partition-table change, bootloader/assets write or server restart.

- New app binary SHA256: f2ada7b071edd9ac133439f83a16e973b446d967a1eeec97e4d8c7e7357799d0
- New app ELF identity: 77ffe48f368415e2f0d7928e7a58d79fc11dc597a986614bd404bc5d0d6cc488
- App validation hash reported by esptool: b3324ea14ed6e483a5a69a1d146353937abb2d86128f9163982162c6a045f993
- New image observed in real robot WebSocket headers at 15:29:32 local time.
- At 15:34:06: 93 pings since 15:29:36, maximum gap 4 seconds. Captured serial: no ws_disconnect, no network-disconnect or chat_source_fault; reconnects=0; heartbeat accepted HTTP 204.
- Supersession/disconnect at 15:29:33 belonged to the previous socket after flashing and is not a post-flash transport failure.
- An acoustic wake attempt using macOS say did not produce a logged wake/state transition; it is not counted as conversation acceptance. Physical LCD appearance and multi-turn post-chat silence still need user/device observation.

The candidate includes existing dirty backend-endpoint configuration edits and preserves them. HEAD eb4d0ed9fd55473dc2066fd434200afe326eae5d alone is not the candidate identity; see fix/source-hashes.json and source-diff.patch. No commit, push or fleet OTA performed.

Evidence: /Users/manhhodinh/Documents/TBOT/task-artifacts/robot-server-unavailable-20261002. Logs: regression-red.log, focused-green.log, pipeline-final.log, liveness-final.log, full-tests.log, full-tests-final.log, mailbox-tsan.log, build.log, backup.log, flash.log, fix/serial-after.log and fix/vps-after.log. The app backup and saved verified new binary are in fix/. To restore this one device, write fix/device-app-before.bin back to 0x20000 via the same esptool command; retain NVS, partition table and assets.

## Physical wake follow-up: explicit upstream billing block

The user reported a disconnect immediately after saying HI ESP. At 15:44:31
Asia/Ho_Chi_Minh, server logs recorded wake-word detection, listen start and a
Google Live open rejected with WebSocket 1011:

> Your prepayment credits are depleted. Please go to AI Studio at https://ai.studio/projects to manage your project and billi

The provider logged fallback_disabled and connection resources were released.
The robot reconnected at 15:44:38; the next Google open failed for the same
billing reason at 15:44:39. A later wake at 15:49:08 and reconnect open at
15:49:15 repeated the same rejection. This is distinct from the missing
firmware keepalive defect. JSON ping/pong continued approximately every three
seconds up to the first wake, and after the reconnect.

A read-only server inspection confirmed running/healthy, zero restarts and
OOM=false. Serial capture after the wake had reconnects=1, uninterrupted
uptime and accepted HTTP 204 management heartbeats; it does not show a device
reboot or establish the device's memory margin under successful conversation.

The provider's current classifier labels this text unknown because it only
recognizes quota/rate/429. The no-fallback path logs the rejection and returns false; the captured
session then released its connection resources. The exact caller that closed
the robot socket was not isolated in this follow-up. Relabeling the log cannot restore Google access and would not alter
the observed open rejection. No additional provider patch, production
configuration change, API-key change, fallback enablement or billing mutation
was made in this follow-up. The project owner must replenish prepaid credits
in AI Studio, then repeat the real wake/conversation/silence trial.

Evidence: fix/vps-wake-failure.log, fix/serial-wake-failure.log and
fix/vps-wake-followup.log. Physical voice acceptance remains BLOCKED; software
regression/build proof and the installed firmware identity above are unchanged.

## Recheck at 16:40–16:41 local time

User requested a fresh check. The server remained running/healthy, restarts=0,
OOM=false. In the preceding 15-minute bounded log capture there were 341 JSON
pings and no new wake or Google billing rejection. Absence of a wake alone
was not treated as proof of restored Google access.

An isolated direct Google Live text-to-audio probe ran inside the current
production container. It loaded current base and private device configuration
through the normal config loaders, then used the production provider's
_get_live_config and GoogleLiveClient. No robot WebSocket, production settings,
API keys, fallback policy, model or service restart were changed. The probe
used gemini-3.1-flash-live-preview, Kore, vi-VN and received 20 audio chunks
(185762 bytes) with audio_end. Config-load-plus-connect took 1018 ms; total
probe 5.71 seconds, cleanup without reported failure. This probe PASS confirms
Google access and a completed generated-audio response at the check time.
It does not prove robot microphone, playback or multi-turn acceptance.

A 12-second read-only USB capture showed source_selected=1, reconnects=2
(stable in the capture), uptime over 4300 seconds, HTTP204 heartbeat, no
captured panic/reboot/disconnect. Server ping continued at16:41:22. The
previous billing rejection is historical; it was not reproduced by this
fresh probe. Actual wake/conversation/silence acceptance now awaits a new
physical robot trial rather than proven ongoing credit depletion.

The first diagnostic script had an incorrect class import and failed before
opening any Live session; corrected to GoogleLiveProvider. Both original and
successful logs are retained: google-recheck-163931.log and
google-recheck-164026.log, plus vps-recheck-163725.log,
serial-recheck-164053.log and vps-recheck-final-164122.log under fix/.
