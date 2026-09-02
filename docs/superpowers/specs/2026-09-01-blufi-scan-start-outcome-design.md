# BluFi Scan Start Outcome Design

## Goal

Make BluFi scan-start rollback authoritative and exception-safe. A catch path must
never resurrect a lease that the coordinator has already completed or released,
and physical/logical start commits must remain one serialized transaction.

## Design

`StartOwnedWifiScan()` returns a no-throw `StartOutcome` describing the exact
ownership state at handoff: whether the lease is still unsubmitted, submitted and
requires recovery, or already completed/released. It also reports any pending
logical request that still needs durable scheduling.

One helper performs every `WifiScanLeaseCoordinator::CommitSubmission()` and
`BlufiWifiScanController::CommitStart()` pair while holding
`wifi_scan_submission_mutex_`. Normal start, start failure, and exception rollback
all use this helper, preventing scan callbacks from entering between the physical
and logical commits.

`TryStartOwnedWifiScanNow()` consumes `StartOutcome` instead of inferring state
from a stale local lease. It republishes only an exact unsubmitted request, requests
recovery only for an exact submitted lease, and does nothing for a completed or
released lease.

## Exception Boundaries

Driver preparation and the ownership transaction are contained inside
`StartOwnedWifiScan() noexcept`. Scheduling a failure response, a pending scan,
list delivery, completion follow-up, or watchdog work is individually guarded so
an exception after lease release cannot flow back into ownership rollback.

## Verification

Native deterministic tests cover callback interleaving between catch-path commits,
exceptions before submission, after driver acceptance but before commit, after
failed-start release, and after inline completion release. Each test asserts the
coordinator/controller terminal state, absence of retry debt, and exact-once start.
Existing normal, ASan/UBSan, TSan, contract, and Xtensa compile checks remain green.
