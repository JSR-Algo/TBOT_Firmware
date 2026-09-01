# Cardputer Blocking Scan Worker Design

## Goal

Make Cardputer blocking Wi-Fi scans fail closed at the global lease boundary and move all driver waits off the Application/LVGL task.

## Ownership Transaction

`WifiManager` reserves the exact Blocking UI lease before radio preparation. Preparation and finish return a tri-state outcome: success, clean failure with proven restoration, or recovery required. A recovery-required outcome retains the external snapshot and manager lifecycle gate. The Blocking UI owner converts the exact coordinator lease to draining debt and routes it through the shared recovery executor. The manager token and global lease are cleared only after role or Idle restoration and `CompleteRecovery` prove the driver boundary safe.

## Worker

One process-lifetime Cardputer scan task owns all calls that may block on scan completion. It accepts a single coalesced request identified by UI generation and revision. UI entry points render the scanning state and submit work; they never call the scan body synchronously. The periodic board poll only attempts allocation-free worker notification and cursor/UI maintenance.

The worker schedules a short immutable completion closure onto the Application task. The UI applies it only when generation and revision still match. Destroying or restarting the UI invalidates older work. Worker creation and notification failures leave the request durable for later polling.

## Verification

Host tests inject every preparation/finish rollback failure and verify global scan acquisition and lifecycle transitions stay blocked until shared recovery succeeds. Worker state tests verify coalescing, stale cancellation, durable creation/notification failure, and that a delayed scan does not prevent unrelated Application work. Existing native sanitizer suites, focused contracts, and exact Cardputer Xtensa objects remain required.
