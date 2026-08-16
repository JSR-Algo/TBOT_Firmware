# T5.4 Passive Listening SD Sync Quiet Verification Evidence

**Date:** 2026-08-16

**Status:** DONE_WITH_CONCERNS - focused and native verification plus the
LCDWiki production build pass; the repository-wide pytest command is blocked
by a pre-existing host dependency collection error, and live CP-7 remains open.

## Pre-Fix Live Failure Signature

```text
lesson asset sync quiet rejected state=5 lesson=0 connect=0 reset=0
lesson asset sync busy or worker unavailable
SD_SYNC_REALTIME_BUSY_TIMEOUT state=LISTENING
```

## Approved Root Cause And Design

At RED capture time, the ESP server considered voice-silent passive realtime
`LISTENING` safe for SD sync while firmware `BeginLessonAssetSyncQuiet()`
rejected every non-idle device state. The approved correction is documented in
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

The pre-fix production function lacked the required `passive_listening`
admission and `IsVoiceDetected()` safety check. This section records the RED
state before the implementation commits below; it does not describe the
current branch source. At RED capture time no production fix existed, and no
CP-7 completion claim was made.

## Implementation Commits

- `f085c1e` (`test(lesson): tighten passive sync quiet contract`) changes the
  worker contract to require the complete passive-listening predicate and the
  ordered quiet-transition sequence (7 insertions, 8 deletions).
- `7b5477e` (`fix(lesson): enter SD sync quiet from passive listening`) admits
  voice-silent passive `LISTENING`, stops listening, clears listening timers,
  disables voice/wake processing, and enters idle before SD sync work (19
  insertions, 3 deletions).

The product implementation SHA verified below is
`7b5477e56eeda4ad73af8c89c2c14d62a7596a8a`. The first evidence commit was
`cad06006035a45e0a27772a45d8b0fffe96bb33b`; the corrected evidence commit is
this commit and is reported in the handoff. Documentation-only evidence commits
are distinct from the product implementation SHA.

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

### Clean-Main Baseline

The preferred clean-main helper command was run first:

```bash
cd /Users/manhhodinh/Documents/TBOT
bash lesson-prod/scripts/verify-on-main.sh \
  /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware -- \
  python3 -m pytest -q
```

At main `98d74f7321f4f96c006a428b7199b4108d8f71a9`, that dependency-empty
temporary worktree exited 2 after 1.33s with three different collection errors
because `TBOT_ESP32_SERVER_REPO` was unset. It did not reach the managed LVGL
collector, so this first attempt was not a comparable baseline (`real 2.35s`).

A private detached main worktree was then populated with the same
`managed_components` dependency tree and run with the canonical manager path,
the same shell PATH, and the same Python 3.14.6 host interpreter:

```bash
git -C /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware \
  worktree add --detach "$baseline_repo" main
cp -cR \
  /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware/.worktrees/t54-face-motion-guard/managed_components \
  "$baseline_repo/managed_components"
cd "$baseline_repo"
TBOT_ESP32_SERVER_REPO=/Users/manhhodinh/Documents/TBOT/robot/esp32-server \
  python3 -m pytest -q
```

Comparable main result: exit 3 after 1.25s (`real 1.45s`), with the same four
errors, missing `/bin/sh: doxygen: command not found`, and
`INTERNALERROR> SystemExit: 32512` in the LVGL `gen_json` collector. The private
worktree was removed after the run. This matching clean-main baseline establishes
that the repository-wide collection failure predates and is not caused by the
T5.4 commits. The prescribed focused project suites and native checks above
pass.

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

The original verification build (`real 118.45s`, SHA-256
`823b81bef95525f2eca38a2acbc865fbd3476e08eb49f18b5dc65bf7f1439936`)
was later superseded by the frozen rebuild below; that prior hash is stale and
must not be used.

After the clean-main baseline, the branch was rebuilt from exact source HEAD
`cad06006035a45e0a27772a45d8b0fffe96bb33b` with the same command and ESP-IDF
Python 3.9 environment:

```bash
export PATH="/usr/bin:$PATH"
./build-lcdwiki.sh --no-flash
```

Result: exit 0 (`real 125.44s`). The build wrapper's board hard-gate reported:

```text
OK: LCDWiki ES3C35P board confirmed in sdkconfig.
BUILD OK: app image .../build/xiaozhi.bin = 3775680 bytes (~3687 KiB).
DONE: built + verified LCDWiki image (skipped flash; --no-flash).
```

Production config auditor command:

```bash
python3 scripts/assert_lcdwiki_prod_config.py sdkconfig
```

Result: exit 0, `LCDWiki production build config OK` (`real 0.06s`).

Artifact metadata commands, run after the auditor and with no later build:

```bash
stat -f 'ARTIFACT_SIZE=%z bytes' build/xiaozhi.bin
stat -f 'ARTIFACT_MTIME=%Sm' -t '%Y-%m-%dT%H:%M:%S%z' build/xiaozhi.bin
shasum -a 256 build/xiaozhi.bin
```

Frozen branch artifact, recorded immediately after the config auditor:

```text
source HEAD: cad06006035a45e0a27772a45d8b0fffe96bb33b
path: /Users/manhhodinh/Documents/TBOT/robot/TBOT-Firmware/.worktrees/t54-face-motion-guard/build/xiaozhi.bin
size: 3775680 bytes
mtime: 2026-08-16T15:59:21+0700
sha256: 7ad59999f7b30bb8ba6625f3307c12d63ba0b39c210a5847cdeeeeecd1bfdb3a
recorded: 2026-08-16T15:59:23+0700
```

This branch artifact is verification-only. The final flash must use a newly
built and audited main artifact after merge. No firmware was flashed, and no
live hardware CP-7 claim is made; that remains a separate device-level
verification step.

## Repository Hygiene

Command:

```bash
git diff --check
```

Result: exit 0 with no output. Before the evidence correction commit,
`git status --short` showed only this owned evidence file; the post-commit clean
status and corrected evidence commit SHA are reported in the handoff.
