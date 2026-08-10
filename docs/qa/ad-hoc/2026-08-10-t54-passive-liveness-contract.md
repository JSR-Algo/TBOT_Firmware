# T5.4 prerequisite — passive WebSocket liveness contract

**Date:** 2026-08-10
**Branch:** `lesson-prod/t54-passive-contract`
**Finding:** F-T54-18

## Repro

```bash
python3 -m pytest tests/test_lesson_passive_websocket_contract.py -q
```

On `main` (`b464030`), one assertion failed because the test extracted a fixed
500-character window around `MaintainPassiveLiveness()`. A safety comment added
before that call moved `!connect_in_flight_.load()` outside the arbitrary window,
even though the guard remains in the same liveness condition.

## Fix

The test now slices the complete passive-liveness block from
`bool passive_liveness_failed` to the following reconnect branch. This keeps all
guard and recovery assertions scoped to the intended block without depending on
comment length.

No firmware runtime code changed.

## Passing rerun

```text
23 passed in 0.06s
```

The repository-wide Python suite remains at its unrelated baseline:
`11 failed / 1204 passed / 1 skipped`; none of those failures is in the changed
contract file.

