# T5.4 Passive Listening SD Sync Quiet RED Evidence

**Date:** 2026-08-16

## Live Failure Signature

```text
lesson asset sync quiet rejected state=5 lesson=0 connect=0 reset=0
lesson asset sync busy or worker unavailable
SD_SYNC_REALTIME_BUSY_TIMEOUT state=LISTENING
```

## Approved Root Cause And Design

The ESP server considers voice-silent passive realtime `LISTENING` safe for SD
sync, while firmware `BeginLessonAssetSyncQuiet()` currently rejects every
non-idle device state. The approved correction is documented in
[Passive Listening SD Sync Quiet Design](../../superpowers/specs/2026-08-16-passive-listening-sd-sync-quiet-design.md).

## Focused RED Regression

Command:

```bash
pytest -q tests/test_lesson_sd_sync_worker_contract.py::test_sync_quiet_admits_only_idle_or_voice_silent_passive_listening
```

The shell did not expose a `pytest` executable (`zsh: command not found:
pytest`), so the same focused test was executed successfully through the
available module entry point:

```bash
python3 -m pytest -q tests/test_lesson_sd_sync_worker_contract.py::test_sync_quiet_admits_only_idle_or_voice_silent_passive_listening
```

Exact failure summary:

```text
FAILED tests/test_lesson_sd_sync_worker_contract.py::test_sync_quiet_admits_only_idle_or_voice_silent_passive_listening
AssertionError: assert 'const DeviceState state = GetDeviceState();' in begin
```

The current production function lacks the required `passive_listening`
admission and `IsVoiceDetected()` safety check. This is RED evidence only: no
production fix exists yet, and no CP-7 completion claim is made.

## Implementation Commits

- `f085c1e` (`test(lesson): tighten passive sync quiet contract`) changes the
  worker contract to require the complete passive-listening predicate and the
  ordered quiet-transition sequence (7 insertions, 8 deletions).
- `7b5477e` (`fix(lesson): enter SD sync quiet from passive listening`) admits
  voice-silent passive `LISTENING`, stops listening, clears listening timers,
  disables voice/wake processing, and enters idle before SD sync work (19
  insertions, 3 deletions).

The implementation tip verified below was
`7b5477e56eeda4ad73af8c89c2c14d62a7596a8a`.

## Focused Verification

Command:

```bash
python3 -m pytest -q \
  tests/test_lesson_sd_sync_worker_contract.py \
  tests/test_lesson_sd_sync_attestation_contract.py \
  tests/test_lesson_sd_sync_no_claim_gate_contract.py \
  tests/test_lesson_sd_sync_watchdog_contract.py \
  tests/test_realtime_voice_state.py \
  tests/test_lesson_passive_websocket_contract.py
```

Result: exit 0, `220 passed in 2.37s` (`real 2.64s`).

Native path command:

```bash
bash scripts/run_host_native_lesson_asset_sync_path_test.sh
```

Result: exit 0, `lesson asset sync path host tests passed` (`real 1.53s`).

Native attestation command:

```bash
bash scripts/run_host_native_lesson_asset_attestation_test.sh
```

Result: exit 0, `lesson asset sync attestation host test OK (22 checks)`
(`real 1.08s`). Clang also emitted six deprecation warnings from compiling
the host cJSON dependency; they did not fail the native test.

## Standard Pytest Suite

Command:

```bash
python3 -m pytest -q
```

Result: exit 3 after 1.31s (`real 1.53s`), with four collection errors. Pytest
descended into the ignored `managed_components/lvgl__lvgl/tests/gen_json`
tree, whose import-time generator failed because `/bin/sh` could not find
`doxygen`:

```text
/bin/sh: doxygen: command not found
TEST FAILED!!
INTERNALERROR> SystemExit: 32512
4 errors in 1.31s
```

This failure occurs in an ignored managed dependency before project test
execution and is caused by the missing host `doxygen` tool, not by either T5.4
commit. The prescribed focused project suites and native checks above pass.

## LCDWiki Production Build

The first exact invocation used the shell-default Python 3.14 and exited 1
before configuration because the matching ESP-IDF virtual environment was not
installed:

```bash
./build-lcdwiki.sh --no-flash
```

```text
ERROR: ESP-IDF Python virtual environment
"/Users/manhhodinh/.espressif/python_env/idf5.5_py3.14_env/bin/python" not found.
```

The existing ESP-IDF 5.5 Python 3.9 environment was then selected without
editing source or configuration, and the same build command was rerun:

```bash
export PATH="/usr/bin:$PATH"
./build-lcdwiki.sh --no-flash
```

Result: exit 0 (`real 118.45s`). The build wrapper's board hard-gate reported:

```text
OK: LCDWiki ES3C35P board confirmed in sdkconfig.
BUILD OK: app image .../build/xiaozhi.bin = 3775680 bytes (~3687 KiB).
DONE: built + verified LCDWiki image (skipped flash; --no-flash).
```

Production config auditor command:

```bash
python3 scripts/assert_lcdwiki_prod_config.py sdkconfig
```

Result: exit 0, `LCDWiki production build config OK` (`real 0.02s`).

Artifact:

```text
path: build/xiaozhi.bin
size: 3775680 bytes
sha256: 823b81bef95525f2eca38a2acbc865fbd3476e08eb49f18b5dc65bf7f1439936
```

No firmware was flashed. No live hardware CP-7 claim is made; that remains a
separate device-level verification step.
