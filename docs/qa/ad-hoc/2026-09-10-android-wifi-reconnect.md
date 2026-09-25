# Android Wi-Fi cancel/reconnect

Status: PASS. Three consecutive Android/robot physical cycles completed.

## Reproduction and changes

The physical LCDWiki ES3C35P (ESP32-S3) panicked on repeated `wifi_setup`.
The original ELF SHA256 starts with `69fd40820`. Decoded stack:
`BTU_StartUp -> BTU_ShutDown -> btu_task_shut_down -> l2c_free`.
Serial evidence captured `heap_alloc_failed size=4864 caps=0x00000804`.

`PrepareWifiConfigEntry` returned success after queuing asynchronous audio and
protocol cleanup. It now retains the original preparation, waits for both owning
workers to finish, and resumes from the application tick. Wake/listen admission
and idle rearm cannot replace that preparation. If automatic recovery becomes
unnecessary because the station reconnects, the board drains and rolls back the
preparation before reserving Bluetooth.

Repeated setup still lost approximately 6 KB of internal RAM per refresh.
`ClaimFetchTask` used `xTaskCreateWithCaps` with a 6144-byte internal stack but
exited with `vTaskDelete`, which does not free the capability-allocated stack and
TCB. The same mismatch existed in `ClaimConfirmationTask` (8192 bytes) and
`CloudReleaseTask` (6144 bytes). All three now use `vTaskDeleteWithCaps`. Their
C++ locals leave an inner scope before self-deletion, so their destructors run.
Scheduled callbacks retain their own captured values.

A temporary outbound-stack PSRAM experiment failed on the third physical cycle
and was removed. The final outbound worker retains its original internal stack.
Earlier candidate cycles are retained as failed-candidate evidence, not acceptance.

Physical testing also found the 3072-byte sleeping credential-fallback task could
not allocate its stack while BLE was active. The existing failure path recovered
online, but acceptance rejected that allocation failure. The 500 ms fallback now
uses a one-shot ESP timer and dispatches to the application task with captured
generation/candidate identity. It rechecks cancellation and duplicate admission
before connecting. Timer/context ownership is released on callback, create
failure, and start failure. The timer callback performs no blocking network work.

## Verification

- Native regression failed before the preparation fix and passes after it.
  It holds an outbound lease, delays audio completion, retries preparation, and
  attempts competing wake/listen/rearm calls without starting duplicate cleanup.
- Board boundary test executes delayed cleanup -> station reconnect -> rollback,
  proving no BLE reservation before cancellation; a fresh explicit entry works.
- Three worker-lifecycle regressions fail on unmatched deletion and pass with
  matched deletion and local destruction before task exit.
- Claim-worker focused regression group: 182 passed.
- Final BluFi/timer focused group: 240 passed. The native timer regression runs
  under ASan/UBSan and covers stale generation before/after callback, stale
  candidate, duplicate, cancellation, timer creation failure and start failure.
- Full suite command: `CHAT_MAILBOX_TSAN=1 python3 -m pytest tests/ -q -rs`.
  Final timer candidate: 1801 passed in 279.62 seconds, no skips.
- Build: ESP-IDF 5.5.4, installed Python 3.9 environment, actual LCDWiki config,
  `idf.py build`: PASS. App-only flash at `0x20000`: PASS, written data hash
  verified; no NVS erase, claim change, or partition-table change.
- Independent review found no correctness blocker in cleanup, matched task
  deletion, C++ lifetime, or one-shot timer ownership.

Exact source, configuration, ELF, application and APK identities, timestamped
physical evidence and per-cycle results are recorded in workspace directory
`task-artifacts/wifi-cancel-reconnect-20260910/`.
The mobile QA report `tbot-mobile/docs/qa/ad-hoc/2026-09-10-wifi-cancel-reconnect.md`
tracks the complete Android/robot journey and known mobile verification gaps.

## Final physical proof

ELF SHA256: `8e450b5dd724670e7b0ff8228a69708d4042d91e4bbe51a8ffc8aa3690456a0b`.
On 2026-09-10, 07:47:04-07:54:43 UTC, all three cycles passed cancellation,
re-entry, fresh IP, accepted HTTP 204 heartbeat and Android online checks.
There was no reset/panic, allocation failure or serial gap between cycles.
Before BLE init, internal free bytes were 27031, 26451, 26671; largest blocks
were 9216, 9216, 8704. No per-cycle 6 KB loss recurred during this run.
The robot was left connected; no ownership-unpair or factory-reset proof is claimed.

## Subsequent ownership-unpair acceptance

The later same-SSID AP selection and server ACK-routing corrections passed
three physical ownership-unpair/re-pair cycles on the final firmware
`94b0db3ee6206bd546bc3e0ff1a2018ac26a67be24c4f64f8f31fc278da43beb`.
All three unpair requests returned HTTP200, then cloud re-claim, fresh IP,
HTTP204 heartbeat and Android online state passed. Final firmware full suite:
1802 passed, no skips. This supplements the earlier cancellation evidence;
see workspace `task-artifacts/unpair-20260910/report.md` and `candidate.json`
for exact deployment identities, timestamps and the existing unrelated test gaps.

## Unpair immediately after reset: final recurrence fix

The user later reproduced409 on a half-open pre-reset robot socket. Server
PING/PONG preflight plus bounded restart readiness now precedes the single
unpair send. Final backend c25bea6/ESP5d188d6b passed3 consecutive physical
early-reset/unpair/re-pair cycles09:57:51-10:03:19UTC. All3 exercised failed
probes and recovery, then one command, ACK, HTTP200, cloud release/re-claim,
fresh IP/heartbeat and Android online. APK/firmware remain unchanged.
See workspace `task-artifacts/unpair-ready-20260910/report.md` for final
identities, timing chain, evidence and prior unrelated test gaps.
