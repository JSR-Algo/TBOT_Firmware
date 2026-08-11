# T5.4 prerequisite — SD reused-counter attestation

Date: 2026-08-11

## Repro

The physical `w02-feelings` run returned a ready SD pack with 11 assets but
reported mutually overlapping counters:

```text
ready=True assetCount=11 downloadedCount=1 skippedCount=10 reusedCount=2 failedCount=0
```

Firmware incremented both `reused` and `skipped` after copying a verified
duplicate asset. The ESP runtime correctly requires downloaded, skipped,
reused, and failed to partition `assetCount`, so the total 13 was rejected.

The regression assertion was changed first to require the reuse branch to
increment only `reusedCount`. It failed against the old source because the
branch still contained `skipped += 1`.

## Fix

Removed the overlapping `skipped` increment from the verified-duplicate reuse
branch in `main/mcp_server.cc`. An asset now contributes to exactly one outcome:
downloaded, skipped because its destination was already valid, reused from an
earlier verified asset in the same pack, or failed.

## Verification

```text
focused RED: 1 failed (unexpected skipped increment present)
focused GREEN: 1 passed
firmware attestation + lesson contracts: 321 passed, 1 skipped
firmware host-native lesson coverage: 2,825/2,825 lines (100%)
ESP runtime + SD pack sync parity: 303 passed
git diff --check: clean
```

Physical verification and T5.4 Ship closeout remain separate steps after the
firmware is merged and flashed onto the attached robot.
