# BluFi Poller Dispatch And Completion Ordering Design

## Goal

Close the final two scan ownership gaps: the WifiManager recovery worker must
never execute a WiFi scan start, and completion must commit logical ownership
before releasing the exact physical lease without making a pending request
externally startable too early.

## Durable Application Dispatch

`BlufiWifiScanRetryState` owns the exact request revision plus transient
`dispatch_enqueuing` and durable `dispatch_scheduled` state. A caller reserves
one unscheduled revision, attempts `Application::Schedule` inside `try/catch`,
and marks it scheduled only after enqueue succeeds. A throw cancels only the
enqueue reservation, leaving the exact request durable and unscheduled for the
WifiManager 50 ms poll fallback.

The WifiManager poll callback may only run this enqueue transaction. The queued
Application callback carries the exact revision and calls the existing start
path, which revalidates the durable revision and lifecycle before acquiring the
global lease or invoking the driver. A lifecycle replacement makes the old
callback a no-op.

## Two-Phase Logical Completion

After callback authentication and AP-list cleanup proof, the controller
prepares the matching completion. Under `wifi_scan_submission_mutex_`, BluFi
commits the prepared logical completion into a retained committed state, then
calls `WifiScanLeaseCoordinator::FinishCompletion` for the exact physical
lease. The retained state still owns the request and cannot promote or expose a
pending scan.

If physical finish succeeds, BluFi releases the committed logical completion
and obtains the pending decision. Only then may it publish/schedule that pending
request. If exact physical finish fails, the controller converts the retained
logical completion to draining recovery state and the coordinator retains the
physical completion debt. Neither side becomes idle and competitors remain
blocked until recovery completes.

## Verification

Native tests inject an Application scheduling throw followed by a successful
executor run, verify that the poller never calls the driver, and verify stale
revision callbacks do nothing. Completion tests assert competitors cannot
acquire the physical lease before logical commit, pending work is not exposed
before both commits, and physical finish failure retains both recovery debts.
Contract tests prohibit direct poller-to-start calls and require logical commit
before physical finish in the serialized production path.
