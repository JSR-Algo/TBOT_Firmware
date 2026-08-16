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
