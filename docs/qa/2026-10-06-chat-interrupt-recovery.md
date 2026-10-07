# Conversation interrupt recovery — 2026-10-06

Scope: current USB LCDWiki ES3C35P robot `14:c1:9f:d1:ac:20`; firmware 2.2.94. This repair includes the pre-existing dirty backend-endpoint and selected-conversation keepalive changes. It does not establish the source identity of the separate morning 2.2.90 unit `14:c1:9f:d1:a8:48`.

## Change

An owned realtime interrupt STOP previously entered recovery sites204/226 and discarded listening intent. The current conversation now retires old output, completes bounded audio cleanup and existing ListenStart delivery, and authorizes the microphone only after current source/epoch/generation and reset/preparation checks. Original deadlines remain; stale/conflicting controls and missing final drain ACK remain fail-closed. Pending/armed logs are emitted once per qualifying recovery.

## Software evidence

- Actual-method interrupt suite:16passed, no skips, ASan/UBSan and TSan, wakeword on/off. Before-fix and publication-clock regressions are saved.
- Related boundary suite:161passed, no skips.
- Full firmware suite:1931passed,2default TSan opt-in skips. Explicit `CHAT_MAILBOX_TSAN=1` rerun plus selected checks:14passed,0skipped; both omitted cases executed.
- Required workflow:4native scripts and140Python checks pass.
- Lesson-production:9reproductions pass; existing `t54-firmware SKIP_REGATE` prevents a full release qualification claim.
- Independent review: no actionable findings; actual-method ASan/UBSan wakeword0/1 cases pass.

## Installed identity

ESP-IDF5.5.4, SDK8e48797f; actual `CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P=y`, ESP32-S3. App3865312bytes fits ota0 of4128768bytes. Partition table matches installed layout. ELF SHA256 `21c36a7e3f350451372845efb94b34369636bca61b89c20a899387a967d851d2`; app image SHA256 `59c6540881e54dae375fb78aa99c1d95b9d59ebf759b7152f7d02938d8834623`.

Full16MiB flash backup retained privately. App-only write at0x20000 completed; hardware app MD5 independently matches the candidate. Immediate pre/post hardware digests for low/NVS/partition, ota1 and assets are unchanged. Original app and partition were separately verified against backup. USB and server reported2.2.94/new ELF at12:36:34+07. No fleet OTA publication.

## Physical observation

Initial post-flash wake/reply completed normal STOP/drain ACK and returned to listening. A later long reply stopped during a simultaneous network loss: server stopped receiving JSON pings at12:37:30, USB backend HTTP TLS timed out at12:38:01 and12:38:21, outbound control deadline hit site216 at12:38:01, server final-drain watchdog expired12:38:06. Passive liveness recovery began12:38:28; new device socket adopted12:38:37 and HTTP204 resumed. No reset was captured. This occurred before the server overlay deployment and does not establish its cause as the interrupt defect. See final bounded physical result below.

## Evidence

Workspace `task-artifacts/robot-disconnect-20261006/fix/`: firmware-verification-summary.md, firmware-full-final.log, firmware-ci.log, firmware-lesson-production.log, firmware-build.log, firmware/review-notes.md, firmware/final-target-rerun-notes.md, firmware-integrated-hashes.json, firmware/device-flash-verification.json, firmware-backup.log, firmware-flash-final.log, serial-firmware-after.private.log, serial-soak.private.log, server-site216.private.log. Private logs/backups can contain device identities or speech and must not be published.

## Final bounded physical result

Postdeploy observation12:48–13:03:55+07 records2Google1008→successful reconnect cycles,0interruptSTOP,0target WebSocketdisconnect,0firmware recoverysites/sourcefaults/reboots.45HTTP204heartbeats in serial windows, selectedsource1/reconnectcounter4unchanged.10normalSTOP and10deviceACKs. Actual macOS speech wake plus two consecutive short questions completed12:58:10–12:58:46 with normal replies/relisten. A13.5s response also completed; one-minute requests produced shorter speech, so original long-response conditions were not qualified. No acoustic-quality/continuous30min/fleetclaim. Existing V6 completedACK regression8pass0skip; unexplained predeployspeech-time transportloss remains detailed in `firmware/site216-investigation.md`. Final report: workspace `task-artifacts/robot-disconnect-20261006/fix/result.md`. Read-only captures intentionally stopped; disposableverifier removed; server finalhealthy/restarts0/OOMfalse.

## Pre-push source integration audit

The fresh main-workspace check reproduced2stale expectations in `tests/test_chat_listen_keepalive.py`. The isolated reviewed candidate already contained the correct assertion: a qualified interrupt must revoke microphone authorization, enter Pending cleanup, leave playout unready, and avoid generic recovery. That test file had been omitted from the earlier integration list. Copied the exact reviewed candidate after dirty-original hash verification; installed firmware source/config/app remain identical. Invalid active-speech/no-intent/source/ID cases retain their assertions. Main interrupt/keepalive sanitizer rerun:6passed0skips. Final main-workspace full suite with `CHAT_MAILBOX_TSAN=1` and actual backend/ESP bindings: **1933passed,0skips**,316.04s. Verification is recorded in workspace `task-artifacts/robot-disconnect-20261006/push/firmware-full-final.log`; release qualification gaps above remain separate.
