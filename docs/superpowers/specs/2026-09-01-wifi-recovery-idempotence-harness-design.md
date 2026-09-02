# Wi-Fi Recovery Idempotence And Behavioral Harness Design

## Goal

Make Station and Config AP radio restoration safe to retry after any partial
driver success, keep owner scans disabled until exact recovery proof is
consumed, and test the production manager recovery state machine
deterministically on the host.

## Root Causes

`RestoreRadioAfterRecovery()` currently sets `scans_enabled_ = true` whenever
the scan session still matches, even when `set_mode`, `set_config`, `start`,
band selection, transmit power, or power-save restoration failed. A retry can
therefore expose scans before exact proof and begins from an unknown partially
started driver state.

`WifiManager::Initialize()` also tears down scanner objects when recovery task
creation fails but leaves the Wi-Fi driver initialized. A second Initialize
attempt then repeats global driver initialization rather than retrying the
failed task stage.

The existing native executor test models manager behavior instead of executing
the production manager scheduling and transition implementation, so it cannot
detect drift in task notification, debt replacement, lifecycle generation, or
lock/callback ordering.

## Architecture

Add a production `WifiRadioRecoveryRestorer` with an injected driver-call
table. The default call table invokes ESP-IDF directly. Every restore attempt
first normalizes the driver with `esp_wifi_stop`; only `ESP_OK` and
`ESP_ERR_WIFI_NOT_STARTED` are benign at that stop stage. It then reapplies the
complete owner mode/configuration, starts Wi-Fi, and reapplies every post-start
setting. No later stage treats an unrelated error as success. Because each
attempt starts from the same stopped state, retry behavior is idempotent after
failure at any stage.

Scanner recovery claims capture the exact scan session and whether that owner
was active when the claim was made. Claiming active recovery gates
`scans_enabled_` false. `RestoreRadioAfterRecovery(claim)` restores only for a
matching active session and never enables scans. `CompleteScanRecovery` first
validates the exact lease and proof with the coordinator, clears the exact
debt, and only then re-enables scans when the captured active session still
matches. A failed proof completion leaves scans gated and permits the manager
to repeat the normalized full restore safely.

## Manager Test Seam

Add a compile-time `TBOT_WIFI_MANAGER_TESTING` seam that exposes dependency
injection and deterministic single-step recovery execution without creating a
second implementation. Production builds retain the same default constructor,
ESP task calls, scanner types, and executor. Host tests compile the actual
`wifi_manager.cc` and inject task creation/notification/delay, scanner recovery
hooks, executor proof generation, and driver restore outcomes.

The seam must only select dependencies and expose state snapshots; scheduling,
debt coalescing/replacement, claim handling, retry decisions, lifecycle
generation checks, pending transition consumption, and callback ordering all
continue through the production methods.

Initialization becomes stage-aware: successful Wi-Fi/scanner setup is retained
when task creation fails, and a later Initialize call retries only task
creation. Public initialization becomes true only after the recovery task is
available.

## Required Behavioral Coverage

The host harness deterministically covers Station and Config AP restoration
failure at stop, mode, AP config, start, band, max transmit power, and power
save, followed by a successful retry. It asserts scans stay gated across every
failure and across a failed exact-proof completion.

The manager harness covers task creation failure and retry, duplicate debt,
replacement by new unclaimed debt, rejection of new debt after claim,
callback-before-claim, executor and restore retry, exact-proof failure,
Station-to-Config and Config-to-Station pending transitions, stale lifecycle
generations, active flags, and callbacks invoked without the manager mutex.
Supported ASan/UBSan and TSan variants run the same production paths.

## Safety Invariants

- No scan begins while recovery debt is claimed or restoration/proof completion
  is incomplete.
- Only exact coordinator proof can clear recovery debt and re-enable an active
  owner's scans.
- Every failed restore leaves scans gated and the driver safe for a full retry.
- A claimed lease is never replaced by another notification.
- Driver, scanner, and lifecycle callbacks run outside the manager mutex.
- Test injection changes dependencies only; it does not fork production state
  transitions or recovery decisions.
