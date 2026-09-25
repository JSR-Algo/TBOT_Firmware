# Wi-Fi Reprovision Lightweight Activation Design

## Problem

Physical Android-to-robot reprovision tests show that Wi-Fi association succeeds,
but the claimed-device recovery path immediately starts an 8 KB activation task,
TLS/WebSocket work, wake-word initialization, and a burst of lesson asset syncs.
On fragmented ESP32-S3 internal RAM this can fail activation task creation and
starve later TLS allocations. Repeated per-asset wake-word initialization also
competes with those network operations.

## Goals

- Keep normal cold-boot activation unchanged.
- Make claimed-device Wi-Fi reprovision use the already persisted runtime config.
- Restore WebSocket, heartbeat, and app-visible online status before optional
  audio work consumes constrained internal RAM.
- Rearm wake-word once after the lesson asset sync burst becomes quiet.
- Preserve automatic setup entry and recovery without pressing BOOT.

## Design

### Lightweight claimed reprovision activation

After BluFi reports a genuine STA connection and the robot is already claimed,
the application transitions from `WifiConfiguring` to `Activating` but does not
spawn the full OTA/config-refresh activation task. It retains or reconstructs the
protocol from persisted settings, completes activation, enters `Idle`, and opens
the passive WebSocket using the normal state-machine path.

Cold boot continues to run the full activation sequence. Unclaimed provisioning
continues to use its existing minimal activation path.

### Debounced wake-word rearm

Starting an asset sync cancels any pending wake-word rearm. Finishing a sync
clears the quiet state and arms a one-shot timer. A later sync restarts the timer.
Only after the timer expires with the robot still claimed, idle, connected, and
outside provisioning does the main task call `RearmClaimedIdleWakeWord()`.

The AFE detection task keeps its 4096-byte stack but allocates that stack from
PSRAM, leaving internal RAM for Wi-Fi and TLS. Failure remains transactional: a
failed AFE/task initialization destroys partial resources and may be retried by a
future valid lifecycle trigger.

## Error Handling

- Ignore stale provisioning completion unless Wi-Fi is actually connected and
  the state is still `WifiConfiguring`.
- Cancel deferred wake rearm when a new sync or Wi-Fi setup starts.
- Recheck all lifecycle guards in the timer callback before enabling wake-word.
- Never report mobile success unless a fresh backend status returns the exact
  requested SSID.

## Verification

- Contract tests prove reprovision does not start the full activation worker.
- Contract tests prove per-sync completion debounces rather than directly rearming.
- Firmware test suite and ESP-IDF build pass.
- Flash the robot and run at least three Android E2E reprovision cycles.
- Each successful cycle must show target SSID receipt, STA IP, passive WebSocket,
  heartbeat HTTP 204, exact-SSID mobile reconciliation, and final
  `wake_running=1` without panic/reboot or repeated allocation failures.
- Exercise cancel-and-retry plus wrong-password recovery without pressing BOOT.

## Non-Goals

- Changing the cold-boot OTA policy.
- Guaranteeing that no future defect can exist; readiness is based on the measured
  automated and physical test evidence above.
