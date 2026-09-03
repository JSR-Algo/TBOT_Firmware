# BluFi Audio Memory Quiesce Design

## Goal

Make Wi-Fi setup reliably discoverable on the LCDWiki ESP32-S3 robot without a
BOOT-button press. Entering setup must release enough internal/DMA SRAM for
Bluedroid to configure and transmit BluFi advertising, while every success,
abort, timeout, and rollback path restores the robot's prior audio availability
exactly once.

## Root Cause

Both automatic Wi-Fi timeout and explicit setup already follow the correct
entry path: `WifiBoard::RequestWifiConfigMode()` schedules
`StartWifiConfigMode()`, which calls `Application::PrepareWifiConfigEntry()` and
`AudioService::BeginWifiProvisioning()` before `Blufi::RestartForSetup()`.

The existing audio preparation only destroys wake-word resources. A claimed
robot also has the audio-input, audio-output, and Opus workers created by
`AudioService::Start()`. Their internal task stacks remain allocated throughout
provisioning. On the attached robot, BluFi reached its init-finish callback with
only 39 bytes of internal DMA heap available. Subsequent 19-29 byte controller
allocations failed with `Memory Full`, so Android scanned normally but never saw
the TBOT advertisement.

## Design

Extend the provisioning audio transaction so it owns both wake-word teardown
and the three base audio workers. `BeginWifiProvisioning()` records whether the
audio service was running, stops it when necessary, waits for all worker handles
to become null within a bounded timeout, and only then returns a valid
provisioning token. BluFi initialization remains forbidden when this quiesce
step fails.

The provisioning token is the sole authority to restore audio. The matching
end/abort operation restarts the service only when that token stopped a
previously running service. Duplicate, stale, or superseded completions cannot
start another worker set. An unclaimed robot that booted with deferred audio
workers remains stopped after provisioning; a claimed robot changing Wi-Fi
returns to its previous running state.

Worker shutdown must be cooperative. `AudioService::Stop()` wakes all blocked
queues/event waits, and the provisioning entry waits on the existing protected
task handles instead of deleting tasks externally. Timeout is fail-closed: do
not initialize BluFi, retain lifecycle ownership needed for a safe recovery,
and report the failed rollback rather than running BLE with insufficient memory.

No Bluetooth heap thresholds, global allocator tuning, LCD behavior, Wi-Fi
credential semantics, NVS data, or claim state changes are part of this fix.

## Lifecycle And Error Handling

1. Prepare the application state and reserve a BluFi provisioning session.
2. Begin the audio provisioning generation and disable wake-word/voice work.
3. If audio was running, stop it and wait a bounded interval for the input,
   output, and Opus task handles to clear.
4. Destroy wake-word resources and finish the provisioning reset.
5. Bind the returned token, initialize BluFi, publish `wifi_configuring`, and
   arm the existing setup timeout.
6. On success, abort, publication failure, initialization failure, or timeout,
   consume the same token and restore audio only if it was running at step 2.

Rollback order remains the reverse of acquisition: tear down or abort BluFi,
restore the audio generation/workers, then restore application state. If worker
shutdown times out, setup does not proceed and the code must not create a second
set of workers.

## Verification

Automated verification follows red-green-refactor:

1. Add a native lifecycle test proving BluFi entry cannot proceed until all
   resident audio workers acknowledge shutdown.
2. Prove a running claimed service restarts exactly once for success, abort,
   timeout, and rollback, while a previously stopped service stays stopped.
3. Prove stale and duplicate tokens cannot restart workers.
4. Run the focused audio, provisioning, BluFi lifecycle, Wi-Fi scan, security,
   and credential-redaction tests, followed by the full firmware test suite.
5. Build the LCDWiki ESP32-S3 application and confirm partition size.

Physical verification uses the attached robot serial log and Android phone:

1. Flash the application image only at offset `0x20000`.
2. Let unavailable Wi-Fi trigger setup automatically without pressing BOOT and
   verify a `TBOT-*` advertisement is visible without allocation failures.
3. Connect to `SUMI_LAU1`, disconnect/unpair, and reconnect three consecutive
   times without rebooting the robot.
4. Switch between `SUMI_LAU1` and `Van Phong Tam Dentist`, including an invalid
   password attempt and BLE interruption, then recover with valid credentials.
5. Confirm every successful connection leaves `wifi_configuring`, restores the
   claimed runtime, and remains reconnectable.

The completed Wi-Fi worktree is removed only after this physical E2E pass.
